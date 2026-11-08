// Discord Rich Presence (see discord_presence.hpp).
//
// Discord's local RPC is a pipe (Windows: \\.\pipe\discord-ipc-N) or a Unix
// socket (<runtime dir>/discord-ipc-N) carrying frames of
//   u32 opcode (little endian), u32 length, JSON
// Opcode 0 is the handshake ({"v":1,"client_id":...}), 1 a command, 2 close.
// Every command gets a reply, which is drained without blocking so a stalled
// Discord can never hold up the game or its shutdown.
#include "discord_presence.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/IniFile.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "shadow_tracker.hpp"

std::string Host_CurrentLocationName();

namespace moderngekko::discord {
namespace {

constexpr std::uint32_t kFightActive = 0x8047B5DA;  // set by _fightInitialize, cleared by _fightFinalize

class Pipe {
public:
  ~Pipe() { Close(); }

  bool Open() {
    Close();
#ifdef _WIN32
    for (int i = 0; i < 10; ++i) {
      const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
      HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        m_handle = h;
        return true;
      }
    }
#else
    std::vector<std::string> dirs;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
      if (const char* v = std::getenv(var); v && *v)
        dirs.push_back(v);
    dirs.push_back("/tmp");
    for (const std::string& dir : dirs) {
      for (const char* sub : {"", "/app/com.discordapp.Discord", "/snap.discord"}) {
        for (int i = 0; i < 10; ++i) {
          const std::string path = dir + sub + "/discord-ipc-" + std::to_string(i);
          sockaddr_un addr{};
          addr.sun_family = AF_UNIX;
          if (path.size() >= sizeof(addr.sun_path))
            continue;
          std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
          const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
          if (fd < 0)
            return false;
          if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            m_fd = fd;
            return true;
          }
          close(fd);
        }
      }
    }
#endif
    return false;
  }

  bool IsOpen() const {
#ifdef _WIN32
    return m_handle != INVALID_HANDLE_VALUE;
#else
    return m_fd >= 0;
#endif
  }

  void Close() {
#ifdef _WIN32
    if (m_handle != INVALID_HANDLE_VALUE)
      CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;
#else
    if (m_fd >= 0)
      close(m_fd);
    m_fd = -1;
#endif
    m_inbox.clear();
  }

  bool Send(std::uint32_t opcode, const std::string& json) {
    std::string frame(8, '\0');
    const std::uint32_t length = static_cast<std::uint32_t>(json.size());
    for (int i = 0; i < 4; ++i) {
      frame[i] = static_cast<char>((opcode >> (8 * i)) & 0xFF);
      frame[4 + i] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    frame += json;
    if (!WriteAll(frame.data(), frame.size())) {
      Close();
      return false;
    }
    return true;
  }

  // Reads whatever has arrived without blocking and returns the complete frames
  // as (opcode, payload). False if the connection dropped.
  bool Drain(std::vector<std::pair<std::uint32_t, std::string>>* frames) {
    char buffer[4096];
    for (;;) {
      const long got = ReadSome(buffer, sizeof(buffer));
      if (got < 0) {
        Close();
        return false;
      }
      if (got == 0)
        break;
      m_inbox.append(buffer, static_cast<std::size_t>(got));
    }
    while (m_inbox.size() >= 8) {
      auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
          v |= std::uint32_t(static_cast<unsigned char>(m_inbox[at + i])) << (8 * i);
        return v;
      };
      const std::uint32_t opcode = u32(0), length = u32(4);
      if (m_inbox.size() < 8 + std::size_t(length))
        break;
      if (frames)
        frames->emplace_back(opcode, m_inbox.substr(8, length));
      m_inbox.erase(0, 8 + std::size_t(length));
    }
    return true;
  }

private:
  bool WriteAll(const char* data, std::size_t size) {
#ifdef _WIN32
    while (size) {
      DWORD wrote = 0;
      if (!WriteFile(m_handle, data, static_cast<DWORD>(size), &wrote, nullptr) || wrote == 0)
        return false;
      data += wrote;
      size -= wrote;
    }
    return true;
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (size) {
      const ssize_t wrote = send(m_fd, data, size, MSG_NOSIGNAL);
      if (wrote > 0) {
        data += wrote;
        size -= static_cast<std::size_t>(wrote);
      } else if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                 std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } else {
        return false;
      }
    }
    return true;
#endif
  }

  // >0 bytes read, 0 nothing waiting, <0 closed.
  long ReadSome(char* buffer, std::size_t size) {
#ifdef _WIN32
    DWORD available = 0;
    if (!PeekNamedPipe(m_handle, nullptr, 0, nullptr, &available, nullptr))
      return -1;
    if (available == 0)
      return 0;
    DWORD got = 0;
    if (!ReadFile(m_handle, buffer, static_cast<DWORD>(std::min<std::size_t>(size, available)), &got,
                  nullptr))
      return -1;
    return static_cast<long>(got);
#else
    const ssize_t got = recv(m_fd, buffer, size, MSG_DONTWAIT);
    if (got > 0)
      return static_cast<long>(got);
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return 0;
    return -1;
#endif
  }

#ifdef _WIN32
  HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
  int m_fd = -1;
#endif
  std::string m_inbox;
};

std::string Json(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
        out += escaped;
      } else {
        out += c;
      }
    }
  }
  return out + "\"";
}

std::uint32_t ProcessId() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<std::uint32_t>(getpid());
#endif
}

struct Activity {
  std::string details, state;
  std::string area;  // art asset key for the place ("" = the game's cover)
  std::string place;
  bool battle = false;
  bool operator==(const Activity&) const = default;
};

// Art asset key for a friendly place name (FriendlyLocationName in
// dolphin_runtime.cpp). Each key is an image uploaded to the Discord
// application's Rich Presence art assets.
std::string AreaAsset(const std::string& place) {
  struct Prefix {
    const char* prefix;
    const char* key;
  };
  static constexpr Prefix kAreas[] = {
      {"Outskirt Stand", "outskirt_stand"},
      {"Snagem Hideout", "snagem_hideout"},
      {"Phenac", "phenac_city"},
      {"Pyrite", "pyrite_town"},
      {"Agate", "agate_village"},
      {"Relic", "agate_village"},
      {"The Under", "the_under"},
      {"Under ", "the_under"},
      {"Deep Colosseum", "the_under"},
      {"Shadow Pok", "shadow_lab"},
      {"Mt. Battle", "mt_battle"},
      {"Realgam", "realgam_tower"},
  };
  for (const Prefix& entry : kAreas)
    if (place.rfind(entry.prefix, 0) == 0)
      return entry.key;
  return {};
}

Activity ReadActivity() {
  Activity a;
  const shadow::Summary shadows = shadow::Read();
  if (!shadows.valid) {
    a.details = "On the title screen";
    return a;
  }
  std::string place = Host_CurrentLocationName();
  if (place.rfind("Room ", 0) == 0)
    place.clear();  // no named area yet (a cutscene or a loading screen)
  a.place = place;
  a.area = AreaAsset(place);

  const std::uint8_t* fight =
      Core::System::GetInstance().GetMemory().GetPointerForRange(kFightActive, 1);
  a.battle = fight && *fight;
  if (a.battle)
    a.details = place.empty() ? "In a battle" : "In a battle \xC2\xB7 " + place;
  else
    a.details = place.empty() ? "Exploring Orre" : place;

  if (shadows.snagged > 0)
    a.state = "Snagged " + std::to_string(shadows.snagged) + "/" + std::to_string(shadows.total) +
              " \xC2\xB7 Purified " + std::to_string(shadows.purified);
  return a;
}

class Presence {
public:
  ~Presence() { Stop(); }  // any exit path that skipped Runtime's Stop()

  void Start(const std::filesystem::path& config_ini) {
    Common::IniFile ini;
    ini.Load(config_ini.string());
    auto* section = ini.GetOrCreateSection("Discord");
    bool enabled = true;
    section->Get("Enabled", &enabled, true);
    section->Get("ApplicationId", &m_client_id, "");
    section->Get("LargeImage", &m_large_image, "colosseum");
    section->Get("AreaImages", &m_area_images, true);
    section->Get("BattleImage", &m_battle_image, "battle");
    if (!enabled)
      return;
    if (m_client_id.empty()) {
      std::fprintf(stderr, "[discord] no [Discord] ApplicationId in config.ini; presence off\n");
      return;
    }
    Stop();
    m_stop = false;
    m_started_at = static_cast<long long>(std::time(nullptr));
    std::fprintf(stderr, "[discord] presence on (application %s)\n", m_client_id.c_str());
    m_thread = std::thread([this] { Run(); });
  }

  void Stop() {
    {
      std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
      m_thread.join();
  }

private:
  bool Connect() {
    if (!m_pipe.Open())
      return false;
    if (!m_pipe.Send(0, "{\"v\":1,\"client_id\":" + Json(m_client_id) + "}"))
      return false;
    // Wait (briefly) for READY; a close frame means the application id was refused.
    for (int i = 0; i < 40 && !Stopping(); ++i) {
      std::vector<std::pair<std::uint32_t, std::string>> frames;
      if (!m_pipe.Drain(&frames)) {
        std::fprintf(stderr, "[discord] Discord dropped the handshake (check [Discord] ApplicationId)\n");
        m_refused = true;
        return false;
      }
      for (const auto& [opcode, payload] : frames) {
        if (opcode == 2) {
          std::fprintf(stderr, "[discord] Discord closed the connection: %s\n", payload.c_str());
          m_pipe.Close();
          m_refused = true;
          return false;
        }
        if (payload.find("\"READY\"") != std::string::npos) {
          std::fprintf(stderr, "[discord] connected\n");
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    m_pipe.Close();
    return false;
  }

  bool SendActivity(const Activity* activity) {
    std::string args = "{\"pid\":" + std::to_string(ProcessId());
    if (activity) {
      args += ",\"activity\":{\"details\":" + Json(activity->details);
      if (!activity->state.empty())
        args += ",\"state\":" + Json(activity->state);
      args += ",\"timestamps\":{\"start\":" + std::to_string(m_started_at) + "}";
      const bool area = m_area_images && !activity->area.empty();
      args += ",\"assets\":{\"large_image\":" + Json(area ? activity->area : m_large_image) +
              ",\"large_text\":" +
              Json(area ? activity->place : std::string("Pok\xC3\xA9mon Colosseum"));
      if (area || (activity->battle && !m_battle_image.empty())) {
        // The small badge: a battle in progress, else the game's cover under the place art.
        const bool battle = activity->battle && !m_battle_image.empty();
        args += ",\"small_image\":" + Json(battle ? m_battle_image : m_large_image) +
                ",\"small_text\":" +
                Json(battle ? std::string("In a battle") : std::string("Pok\xC3\xA9mon Colosseum"));
      }
      args += "}}";
    }
    args += "}";
    const std::string command = "{\"cmd\":\"SET_ACTIVITY\",\"args\":" + args +
                                ",\"nonce\":\"" + std::to_string(++m_nonce) + "\"}";
    return m_pipe.Send(1, command);
  }

  bool Stopping() {
    std::lock_guard lock(m_mutex);
    return m_stop;
  }

  void Run() {
    using clock = std::chrono::steady_clock;
    clock::time_point next_connect{}, last_send{};
    Activity sent;
    bool have_sent = false;
    for (;;) {
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::seconds(2), [this] { return m_stop; });
        if (m_stop)
          break;
      }
      const auto now = clock::now();
      if (!m_pipe.IsOpen()) {
        if (now < next_connect)
          continue;
        // A refused id won't start working; retry rarely in case Discord was mid-update.
        next_connect = now + std::chrono::seconds(m_refused ? 300 : 15);
        if (!Connect())
          continue;
        have_sent = false;
      }
      std::vector<std::pair<std::uint32_t, std::string>> replies;
      if (!m_pipe.Drain(&replies))
        continue;
      for (const auto& reply : replies)
        if (reply.second.find("\"evt\":\"ERROR\"") != std::string::npos)
          std::fprintf(stderr, "[discord] Discord refused the update: %s\n", reply.second.c_str());
      const Activity activity = ReadActivity();
      // Discord rate-limits activity updates (about 5 per 20 s).
      if ((!have_sent || !(activity == sent)) && now - last_send >= std::chrono::seconds(5)) {
        if (SendActivity(&activity)) {
          std::fprintf(stderr, "[discord] status: %s | %s | art %s%s\n", activity.details.c_str(),
                       activity.state.c_str(),
                       activity.area.empty() ? m_large_image.c_str() : activity.area.c_str(),
                       activity.battle ? " + battle badge" : "");
          sent = activity;
          have_sent = true;
          last_send = now;
        }
      }
    }
    if (m_pipe.IsOpen()) {
      SendActivity(nullptr);
      m_pipe.Close();
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_wake;
  bool m_stop = false;
  std::thread m_thread;
  Pipe m_pipe;
  std::string m_client_id, m_large_image, m_battle_image;
  bool m_area_images = true;
  long long m_started_at = 0;
  unsigned m_nonce = 0;
  bool m_refused = false;
};

Presence& Instance() {
  static Presence presence;
  return presence;
}

}  // namespace

void Start(const std::filesystem::path& config_ini) { Instance().Start(config_ini); }
void Stop() { Instance().Stop(); }

}  // namespace moderngekko::discord
