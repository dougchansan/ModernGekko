#include "quick_boot.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace moderngekko::quick_boot {
namespace {

namespace fs = std::filesystem;

// Bump when the snapshot's meaning changes (what the title state must hold).
constexpr const char *kVersion = "quick-boot-1";

struct Hash {
  std::uint64_t value = 0xcbf29ce484222325ull;
  void Bytes(const void *data, std::size_t size) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i)
      value = (value ^ p[i]) * 0x100000001b3ull;
  }
  void Text(const std::string &text) {
    Bytes(text.data(), text.size());
    Bytes("\0", 1);
  }
  void Number(std::uint64_t number) { Bytes(&number, sizeof(number)); }
};

// Binaries: path, size and write time (hashing them would cost more than the
// boot it saves).
void Stamp(Hash &hash, const fs::path &path) {
  std::error_code ec;
  hash.Text(path.generic_string());
  hash.Number(fs::file_size(path, ec));
  hash.Number(static_cast<std::uint64_t>(fs::last_write_time(path, ec).time_since_epoch().count()));
}

void StampTree(Hash &hash, const fs::path &root) {
  std::error_code ec;
  if (fs::is_regular_file(root, ec)) {
    Stamp(hash, root);
    return;
  }
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec))
      Stamp(hash, it->path());
}

// Small files whose bytes matter (memory card saves, patch ini): contents.
void ContentTree(Hash &hash, const fs::path &root) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec))
      continue;
    hash.Text(fs::relative(it->path(), root, ec).generic_string());
    std::ifstream in(it->path(), std::ios::binary);
    char buffer[1 << 16];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
      hash.Bytes(buffer, static_cast<std::size_t>(in.gcount()));
  }
}

std::mutex s_mutex;
std::atomic<bool> s_loaded{false};
fs::path s_state, s_key_file;
std::string s_key;
bool s_want_snapshot = false;
bool s_saving = false;
std::chrono::steady_clock::time_point s_save_time;
std::uintmax_t s_last_size = 0;

}  // namespace

std::optional<fs::path> Prepare(const fs::path &user_directory, const std::vector<fs::path> &inputs,
                                bool skip_intro) {
  const char *env = std::getenv("MODERNGEKKO_QUICK_BOOT");
  if (!skip_intro || (env && env[0] == '0'))
    return std::nullopt;

  Hash hash;
  hash.Text(kVersion);
#ifdef _WIN32
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
    Stamp(hash, fs::path(exe));
#endif
  for (const auto &input : inputs)
    StampTree(hash, input);
  ContentTree(hash, user_directory / "GC");
  ContentTree(hash, user_directory / "GameSettings");
  char key[32];
  std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash.value));

  std::lock_guard lock(s_mutex);
  const fs::path directory = user_directory / "StartupCache";
  s_state = directory / "title.sav";
  s_key_file = directory / "title.key";
  s_key = key;

  std::string stored;
  if (std::ifstream in(s_key_file); in)
    std::getline(in, stored);
  std::error_code ec;
  if (stored == s_key && fs::file_size(s_state, ec) > 0 && !ec) {
    s_loaded = true;
    std::fprintf(stderr, "[quick-boot] booting from the title snapshot (%s)\n", key);
    return s_state;
  }
  // Missing or stale: boot normally and save a fresh one at the title.
  fs::remove(s_key_file, ec);
  fs::create_directories(directory, ec);
  s_want_snapshot = true;
  std::fprintf(stderr, "[quick-boot] no valid snapshot (%s); one is saved at the title\n", key);
  return std::nullopt;
}

bool Loaded() { return s_loaded.load(std::memory_order_relaxed); }

void TitleReached() {
  std::lock_guard lock(s_mutex);
  if (!s_want_snapshot)
    return;
  s_want_snapshot = false;
  s_saving = true;
  s_save_time = std::chrono::steady_clock::now();
  s_last_size = 0;
  State::SaveAs(Core::System::GetInstance(), s_state.string());
  std::fprintf(stderr, "[quick-boot] saving the title snapshot\n");
}

void Tick() {
  std::lock_guard lock(s_mutex);
  if (s_loaded) {
    // Booting from the snapshot should look like a boot: no "Loaded State"
    // or "Decompressing State" messages over the title.
    static const auto first = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - first < std::chrono::seconds(3))
      OSD::ClearMessages();
  }
  if (!s_saving)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (now - s_save_time < std::chrono::seconds(2))
    return;
  // The state is compressed and written on a worker thread: valid once its
  // size has stopped changing.
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(s_state, ec);
  if (ec || size == 0 || size != s_last_size) {
    s_last_size = ec ? 0 : size;
    s_save_time = now - std::chrono::milliseconds(1500);
    return;
  }
  s_saving = false;
  std::ofstream out(s_key_file, std::ios::trunc);
  out << s_key << '\n';
  std::fprintf(stderr, "[quick-boot] title snapshot saved (%ju bytes)\n", size);
}

}  // namespace moderngekko::quick_boot
