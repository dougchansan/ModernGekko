#include "attract_video.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

namespace moderngekko::attract {
namespace {

// Guest movie state (research/attract/playback/REPORT.md): both active bytes
// set while a movie plays; the open wrapper keeps the file name pointer; the
// THP player's displayed frame = displayed set's frame + start offset.
constexpr std::uint32_t kMovieActive = 0x8047B440;  // and +1
constexpr std::uint32_t kMovieName = 0x8047B448;
constexpr std::uint32_t kPlayer = 0x8046AC60;
constexpr const char *kAttractName = "movie/autodemo01.thp";
constexpr double kGuestFps = 29.97;

struct GuestMovie {
  bool attract = false;
  int frame = -1;
};

GuestMovie ReadGuest() {
  GuestMovie movie;
  auto &memory = Core::System::GetInstance().GetMemory();
  auto bytes = [&memory](std::uint32_t address, std::uint32_t size) -> const std::uint8_t * {
    return address >= 0x80000000u ? memory.GetPointerForRange(address, size) : nullptr;
  };
  auto u32 = [&bytes](std::uint32_t address) -> std::uint32_t {
    const auto *p = bytes(address, 4);
    return p ? (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                   (std::uint32_t(p[2]) << 8) | p[3]
             : 0;
  };
  const auto *active = bytes(kMovieActive, 2);
  if (!active || !active[0] || !active[1])
    return movie;
  const std::size_t name_size = std::strlen(kAttractName);
  const auto *name = bytes(u32(kMovieName), static_cast<std::uint32_t>(name_size + 1));
  if (!name)
    return movie;
  // Accept an optional leading "files/" or "/" (the decomp passes the bare path).
  std::string text(reinterpret_cast<const char *>(name), name_size + 1);
  text.resize(std::strlen(text.c_str()));
  if (text != kAttractName)
    return movie;
  movie.attract = true;
  const auto *state = bytes(kPlayer + 0xA4, 1);
  const std::uint32_t displayed = u32(kPlayer + 0xE8);
  if (u32(kPlayer + 0xA0) && state && *state && displayed)
    movie.frame = static_cast<std::int32_t>(u32(displayed + 0x0C)) +
                  static_cast<std::int32_t>(u32(kPlayer + 0xC0));
  return movie;
}

struct Frame {
  double time = 0;  // seconds
  std::vector<std::uint8_t> pixels;  // BGRA, alpha 255
};

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// Decodes the replacement video on its own thread, a few frames ahead.
class Decoder {
public:
  explicit Decoder(std::wstring path) : m_path(std::move(path)) {
    m_thread = std::thread([this] { Run(); });
  }
  ~Decoder() {
    {
      std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    m_space.notify_all();
    m_thread.join();
  }

  int Width() const { return m_width.load(); }
  int Height() const { return m_height.load(); }
  bool Failed() const { return m_failed.load(); }

  // The latest decoded frame at or before `time`, or null if none is due yet.
  std::unique_ptr<Frame> Take(double time) {
    std::unique_ptr<Frame> chosen;
    {
      std::lock_guard lock(m_mutex);
      while (!m_frames.empty() && m_frames.front()->time <= time) {
        chosen = std::move(m_frames.front());
        m_frames.pop_front();
      }
    }
    if (chosen)
      m_space.notify_all();
    return chosen;
  }

private:
  void Run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      m_failed = true;
      if (SUCCEEDED(com))
        CoUninitialize();
      return;
    }
    Decode();
    MFShutdown();
    if (SUCCEEDED(com))
      CoUninitialize();
  }

  void Decode() {
    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 2);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(m_path.c_str(), attributes.Get(), &reader))) {
      std::fprintf(stderr, "[attract] cannot open the replacement video\n");
      m_failed = true;
      return;
    }
    const DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(video, TRUE);
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(video, nullptr, type.Get()))) {
      std::fprintf(stderr, "[attract] the video cannot be decoded to RGB32\n");
      m_failed = true;
      return;
    }
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(video, &current);
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
    m_width = static_cast<int>(width);
    m_height = static_cast<int>(height);
    std::fprintf(stderr, "[attract] replacement video %ux%u\n", width, height);

    for (;;) {
      {
        std::unique_lock lock(m_mutex);
        m_space.wait(lock, [this] { return m_stop || m_frames.size() < 4; });
        if (m_stop)
          return;
      }
      DWORD flags = 0;
      LONGLONG timestamp = 0;
      ComPtr<IMFSample> sample;
      if (FAILED(reader->ReadSample(video, 0, nullptr, &flags, &timestamp, &sample)) ||
          (flags & MF_SOURCE_READERF_ENDOFSTREAM))
        return;  // the last frame stays up until the game's movie ends
      if (!sample)
        continue;
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        continue;
      auto frame = std::make_unique<Frame>();
      frame->time = static_cast<double>(timestamp) / 1e7;
      frame->pixels.resize(static_cast<std::size_t>(width) * height * 4);
      ComPtr<IMF2DBuffer> buffer2d;
      BYTE *scan0 = nullptr;
      LONG pitch = 0;
      BYTE *data = nullptr;
      DWORD length = 0;
      const bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) &&
                            SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
      if (!locked2d) {
        if (FAILED(buffer->Lock(&data, nullptr, &length)))
          continue;
        scan0 = data;
        pitch = static_cast<LONG>(width * 4);
      }
      const std::size_t row = static_cast<std::size_t>(width) * 4;
      for (UINT32 y = 0; y < height; ++y) {
        std::uint8_t *out = frame->pixels.data() + y * row;
        std::memcpy(out, scan0 + static_cast<std::ptrdiff_t>(y) * pitch, row);
        for (std::size_t x = 3; x < row; x += 4)
          out[x] = 0xFF;  // RGB32's fourth byte is padding, not alpha
      }
      if (locked2d)
        buffer2d->Unlock2D();
      else
        buffer->Unlock();
      std::lock_guard lock(m_mutex);
      m_frames.push_back(std::move(frame));
    }
  }

  std::wstring m_path;
  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_space;
  std::deque<std::unique_ptr<Frame>> m_frames;
  bool m_stop = false;
  std::atomic<int> m_width{0}, m_height{0};
  std::atomic<bool> m_failed{false};
};
#endif

struct Player {
#ifdef _WIN32
  std::unique_ptr<Decoder> decoder;
#endif
  std::unique_ptr<AbstractTexture> texture;
  bool shown = false;  // a replacement frame is uploaded for this playback
  int last_frame = -1;
  std::chrono::steady_clock::time_point frame_changed;
};
Player s_player;

}  // namespace

void Draw() {
#ifdef _WIN32
  Player &player = s_player;
  const GuestMovie movie = ReadGuest();
  if (!movie.attract || movie.frame < 0) {
    if (player.decoder) {
      player.decoder.reset();  // joins the decoder thread
      player.shown = false;
      player.last_frame = -1;
      std::fprintf(stderr, "[attract] replacement stopped\n");
    }
    return;
  }
  if (!player.decoder) {
    const std::string path = File::GetUserPath(D_LOAD_IDX) + "Attract/autodemo01.mp4";
    if (!File::Exists(path))
      return;
    player.decoder = std::make_unique<Decoder>(UTF8ToWString(path));
    player.shown = false;
    player.last_frame = -1;
    std::fprintf(stderr, "[attract] replacement started\n");
  }
  if (player.decoder->Failed())
    return;  // the game's own movie stays visible

  // The game's displayed frame is the clock; between its 29.97 Hz frames the
  // time advances smoothly (held if the game stops advancing, e.g. paused).
  const auto now = std::chrono::steady_clock::now();
  if (movie.frame != player.last_frame) {
    player.last_frame = movie.frame;
    player.frame_changed = now;
  }
  const double since =
      std::min(std::chrono::duration<double>(now - player.frame_changed).count(), 1.0 / kGuestFps);
  const double time = movie.frame / kGuestFps + since;

  if (auto frame = player.decoder->Take(time)) {
    const int width = player.decoder->Width(), height = player.decoder->Height();
    if (!player.texture || static_cast<int>(player.texture->GetWidth()) != width ||
        static_cast<int>(player.texture->GetHeight()) != height) {
      const TextureConfig config(width, height, 1, 1, 1, AbstractTextureFormat::BGRA8, 0,
                                 AbstractTextureType::Texture_2DArray);
      player.texture = g_gfx->CreateTexture(config, "Attract replacement");
    }
    if (player.texture) {
      player.texture->Load(0, width, height, width, frame->pixels.data(), frame->pixels.size());
      player.shown = true;
    }
  }
  if (!player.shown || !player.texture)
    return;

  // Fit the video to the window (black bars if the aspect differs).
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 255));
  const float video_aspect =
      static_cast<float>(player.texture->GetWidth()) / static_cast<float>(player.texture->GetHeight());
  float w = display.x, h = display.x / video_aspect;
  if (h > display.y) {
    h = display.y;
    w = display.y * video_aspect;
  }
  const ImVec2 a((display.x - w) * 0.5f, (display.y - h) * 0.5f);
  draw->AddImage(*player.texture, a, ImVec2(a.x + w, a.y + h));
#endif
}

}  // namespace moderngekko::attract
