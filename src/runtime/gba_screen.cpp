#include "gba_screen.hpp"
#include "gba_screen_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <imgui.h>

#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"

// Kept out of dolphin_runtime.cpp: mGBA's headers define macros (ROR, ...)
// that break Dolphin's x64 emitter header included there.
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif

namespace moderngekko::gba_screen
{
namespace
{
struct Screen
{
  std::mutex mutex;
  std::vector<u32> pixels;  // RGBA bytes, as the core writes them
  u32 width = 0;
  u32 height = 0;
  u64 frame = 0;  // counts the frames the core has finished
  bool fresh = false;
  bool active = false;
  std::unique_ptr<AbstractTexture> texture;  // video thread only
  const AbstractGfx* texture_gfx = nullptr;  // the backend that made it
};

// Never destroyed: a texture must not be freed after its video backend is.
std::array<Screen, 4>& s_screens = *new std::array<Screen, 4>();

#ifdef HAS_LIBMGBA
// Each port's running GBA, for ResetGba (guarded by that port's screen mutex).
std::array<std::weak_ptr<HW::GBA::Core>, 4> s_cores;
#endif

std::mutex s_settings_mutex;
Settings s_settings;
std::function<void()> s_changed;

// Set when a panel must be put back at its stored place on the next frame
// (moved back from its own window, or its place reset).
std::array<std::atomic<bool>, 4> s_place_again{};

// Player colours, as on Colosseum's own player screen.
constexpr std::array<ImU32, 4> kPlayerColors = {
    IM_COL32(222, 58, 58, 255), IM_COL32(58, 112, 232, 255),
    IM_COL32(232, 186, 34, 255), IM_COL32(52, 176, 88, 255)};

void NotifyChanged()
{
  std::function<void()> callback;
  {
    std::lock_guard lock(s_settings_mutex);
    callback = s_changed;
  }
  if (callback)
    callback();
}

#ifdef HAS_LIBMGBA
class Host final : public GBAHostInterface
{
public:
  explicit Host(std::weak_ptr<HW::GBA::Core> core) : m_core(std::move(core)) {}
  ~Host() override
  {
    if (m_port < 0)
      return;
    std::lock_guard lock(s_screens[m_port].mutex);
    s_screens[m_port].active = false;
  }

  void GameChanged() override
  {
    if (const auto core = m_core.lock())
    {
      const HW::GBA::CoreInfo info = core->GetCoreInfo();
      if (info.device_number < 0 || info.device_number >= 4)
        return;
      m_port = info.device_number;
      std::lock_guard lock(s_screens[m_port].mutex);
      s_cores[m_port] = m_core;
      s_screens[m_port].width = info.width;
      s_screens[m_port].height = info.height;
    }
  }

  void FrameEnded(std::span<const u32> video_buffer) override
  {
    if (m_port < 0)
      GameChanged();
    if (m_port < 0)
      return;
    Screen& screen = s_screens[m_port];
    std::lock_guard lock(screen.mutex);
    if (video_buffer.size() < static_cast<std::size_t>(screen.width) * screen.height)
      return;
    screen.pixels.assign(video_buffer.begin(),
                         video_buffer.begin() + static_cast<std::ptrdiff_t>(screen.width) *
                                                    screen.height);
    for (u32& pixel : screen.pixels)
      pixel |= 0xFF000000u;  // the core leaves alpha clear
    ++screen.frame;
    screen.fresh = true;
    screen.active = true;
  }

private:
  std::weak_ptr<HW::GBA::Core> m_core;
  int m_port = -1;
};
#endif

void UploadTexture(Screen& screen)
{
  std::lock_guard lock(screen.mutex);
  // A new runtime brings a new backend; the old texture died with its own.
  if (screen.texture && screen.texture_gfx != g_gfx.get())
    static_cast<void>(screen.texture.release());
  if (!screen.active || !screen.fresh || !g_gfx)
    return;
  if (!screen.texture || screen.texture->GetWidth() != screen.width ||
      screen.texture->GetHeight() != screen.height)
  {
    const TextureConfig config(screen.width, screen.height, 1, 1, 1,
                               AbstractTextureFormat::RGBA8, 0,
                               AbstractTextureType::Texture_2DArray);
    screen.texture = g_gfx->CreateTexture(config, "GBA screen");
    screen.texture_gfx = g_gfx.get();
  }
  if (screen.texture)
    screen.texture->Load(0, screen.width, screen.height, screen.width,
                         reinterpret_cast<const u8*>(screen.pixels.data()),
                         screen.pixels.size() * sizeof(u32));
  screen.fresh = false;
}

// Players 1 and 3 down the left, 2 and 4 down the right; 1 and 2 low, where
// the GBAs of a two-player game sit.
ImVec2 DefaultPlace(int port, ImVec2 size, ImVec2 display)
{
  constexpr float margin = 12.0f;
  const bool right = (port % 2) == 1;
  const bool low = port < 2;
  return ImVec2(right ? display.x - size.x - margin : margin,
                low ? display.y - size.y - margin : margin);
}

void KeepAspect(ImGuiSizeCallbackData* data)
{
  const float aspect = *static_cast<const float*>(data->UserData);
  data->DesiredSize.y = std::round(data->DesiredSize.x / aspect);
}

void Badge(ImDrawList* draw, ImVec2 at, int port, float scale, ImU32 alpha)
{
  const std::string label = "P" + std::to_string(port + 1);
  const char* text = label.c_str();
  const float font = ImGui::GetFontSize() * scale;
  const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font, 1e9f, 0.0f, text);
  const ImVec2 pad(font * 0.35f, font * 0.15f);
  const ImU32 color = (kPlayerColors[port] & 0x00FFFFFFu) | (alpha << 24);
  draw->AddRectFilled(at, ImVec2(at.x + size.x + pad.x * 2, at.y + size.y + pad.y * 2), color,
                      font * 0.3f);
  draw->AddText(ImGui::GetFont(), font, ImVec2(at.x + pad.x, at.y + pad.y),
                IM_COL32(255, 255, 255, alpha), text);
}
}  // namespace

Settings GetSettings()
{
  std::lock_guard lock(s_settings_mutex);
  return s_settings;
}

void SetSettings(const Settings& settings)
{
  std::lock_guard lock(s_settings_mutex);
  s_settings = settings;
  for (auto& view : s_settings.views)
    view.opacity = std::clamp(view.opacity, 10, 100);
  for (auto& again : s_place_again)
    again = true;
}

void SetChangedCallback(std::function<void()> callback)
{
  std::lock_guard lock(s_settings_mutex);
  s_changed = std::move(callback);
}

bool IsActive(int port)
{
  if (port < 0 || port >= 4)
    return false;
  std::lock_guard lock(s_screens[port].mutex);
  return s_screens[port].active;
}

void Draw()
{
  for (Screen& screen : s_screens)
    UploadTexture(screen);

  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 display = io.DisplaySize;
  if (display.x < 64 || display.y < 64)
    return;
  static ImVec2 s_last_display;
  const bool display_changed = display.x != s_last_display.x || display.y != s_last_display.y;
  s_last_display = display;

  Settings settings = GetSettings();
  bool moved = false, save_now = false;
  static bool s_unsaved_move = false;
  static std::array<float, 4> s_aspect{};

  for (int port = 0; port < 4; ++port)
  {
    Screen& screen = s_screens[port];
    View& view = settings.views[port];
    if (!screen.active || !screen.texture || view.detached)
      continue;
    const float tex_w = static_cast<float>(screen.texture->GetWidth());
    const float tex_h = static_cast<float>(screen.texture->GetHeight());
    s_aspect[port] = tex_w / tex_h;

    // A third of the window's height by default: big enough to read, small
    // enough to leave the battle visible behind it.
    const float width = view.width > 0.0f ? view.width * display.x :
                                            std::round(display.y * 0.30f * s_aspect[port]);
    const ImVec2 size(width, std::round(width / s_aspect[port]));
    ImVec2 place = view.x < 0.0f ? DefaultPlace(port, size, display) :
                                   ImVec2(view.x * display.x, view.y * display.y);
    place.x = std::clamp(place.x, 0.0f, std::max(0.0f, display.x - size.x));
    place.y = std::clamp(place.y, 0.0f, std::max(0.0f, display.y - size.y));

    const bool place_again = s_place_again[port].exchange(false) || display_changed;
    ImGui::SetNextWindowPos(place, place_again ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, place_again ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(96.0f, 96.0f / s_aspect[port]), display, KeepAspect,
                                        &s_aspect[port]);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    const std::string name = "##gba" + std::to_string(port + 1);
    ImGui::Begin(name.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(3);

    const ImVec2 at = ImGui::GetWindowPos();
    const ImVec2 now = ImGui::GetWindowSize();
    const ImVec2 end(at.x + now.x, at.y + now.y);
    const ImU32 alpha = static_cast<ImU32>(std::lround(view.opacity * 2.55));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddImage(*screen.texture, at, end, ImVec2(0, 0), ImVec2(1, 1),
                   IM_COL32(255, 255, 255, alpha));
    draw->AddRect(at, end, (kPlayerColors[port] & 0x00FFFFFFu) | (alpha << 24), 0.0f, 0, 2.0f);
    if (settings.show_numbers)
      Badge(draw, ImVec2(at.x + 6, at.y + 6), port, std::max(1.0f, now.y / 240.0f), alpha);
    ImGui::Dummy(now);

    if (ImGui::BeginPopupContextWindow("gba"))
    {
      ImGui::TextDisabled("Game Boy Advance - Player %d", port + 1);
      ImGui::Separator();
      const bool netplay = NetPlay::IsNetPlayRunning();
      if (ImGui::MenuItem("Reset GBA (off and on)", "L+R+Start+Select", false, !netplay))
        ResetGba(port);
      int opacity = view.opacity;
      ImGui::SetNextItemWidth(160.0f);
      if (ImGui::SliderInt("Opacity", &opacity, 10, 100, "%d%%"))
      {
        view.opacity = (opacity + 2) / 5 * 5;
        moved = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        save_now = true;
      if (ImGui::MenuItem("Show player numbers", nullptr, settings.show_numbers))
      {
        settings.show_numbers = !settings.show_numbers;
        save_now = true;
      }
#if defined(_WIN32) || defined(__APPLE__)
      if (ImGui::MenuItem("Move to its own window"))
      {
        view.detached = true;
        view.opacity = 100;  // a window of its own covers nothing
        save_now = true;
      }
#endif
      if (ImGui::MenuItem("Reset size and place"))
      {
        view.x = view.y = -1.0f;
        view.width = 0.0f;
        s_place_again[port] = true;
        save_now = true;
      }
      ImGui::EndPopup();
    }
    else if (!place_again && (std::abs(now.x - size.x) > 0.5f || std::abs(at.x - place.x) > 0.5f ||
                              std::abs(at.y - place.y) > 0.5f))
    {
      // Dragged or resized: remember it as a share of the window.
      view.x = at.x / display.x;
      view.y = at.y / display.y;
      view.width = now.x / display.x;
      moved = true;
    }
    ImGui::End();
  }

  if (moved || save_now)
  {
    std::lock_guard lock(s_settings_mutex);
    s_settings.views = settings.views;
    s_settings.show_numbers = settings.show_numbers;
  }
  if (moved)
    s_unsaved_move = true;
  // Save once the mouse lets go, not on every frame of a drag.
  if (save_now || (s_unsaved_move && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))
  {
    s_unsaved_move = false;
    NotifyChanged();
  }
}

bool ResetGba(int port)
{
#ifdef HAS_LIBMGBA
  if (port < 0 || port >= 4 || NetPlay::IsNetPlayRunning())
    return false;
  std::weak_ptr<HW::GBA::Core> core;
  {
    std::lock_guard lock(s_screens[port].mutex);
    core = s_cores[port];
  }
  if (core.expired())
    return false;
  std::fprintf(stderr, "[gba] port %d: power cycled\n", port + 1);
  // Asked for from the video thread (a GBA screen's menu), the window thread or
  // automation; pausing the CPU is only safe from the window thread.
  ::Core::QueueHostJob([core](::Core::System& system) {
    ::Core::RunOnCPUThread(system, [core] {
      if (const auto running = core.lock())
        running->Reset();
    });
  });
  return true;
#else
  return false;
#endif
}

void Tick()
{
  detail::TickWindows();

  // Holding L+R+Start+Select on a GBA's controller for a second turns that
  // GBA off and on. Not over netplay: reading the pad here takes Dolphin's
  // pending reset (the X bit netplay must send with the inputs), and putting
  // it back re-armed it forever, so the GBA kept rebooting and could not read
  // its cartridge's save. There, the Players menu and automation reset it.
  static auto s_last_poll = std::chrono::steady_clock::time_point{};
  static std::array<std::chrono::steady_clock::time_point, 4> s_held_since{};
  static std::array<bool, 4> s_fired{};
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last_poll < std::chrono::milliseconds(16) || !Pad::IsGBAInitialized())
    return;
  s_last_poll = now;
  if (NetPlay::IsNetPlayRunning())
    return;
  constexpr u16 combo = PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z | PAD_BUTTON_START;
  for (int i = 0; i < 4; ++i)
  {
    if (!IsActive(i))
    {
      s_held_since[i] = {};
      s_fired[i] = false;
      continue;
    }
    const GCPadStatus status = Pad::GetGBAStatus(i);
    if ((status.button & combo) != combo)
    {
      s_held_since[i] = {};
      s_fired[i] = false;
      continue;
    }
    if (s_held_since[i] == std::chrono::steady_clock::time_point{})
      s_held_since[i] = now;
    else if (!s_fired[i] && now - s_held_since[i] >= std::chrono::seconds(1))
    {
      s_fired[i] = true;
      ResetGba(i);
    }
  }
}

namespace detail
{
bool CopyFrame(int port, std::vector<u32>* bgra, u32* width, u32* height, u64* frame)
{
  if (port < 0 || port >= 4)
    return false;
  Screen& screen = s_screens[port];
  std::lock_guard lock(screen.mutex);
  if (!screen.active || screen.pixels.empty())
    return false;
  if (screen.frame == *frame)
    return true;
  *frame = screen.frame;
  *width = screen.width;
  *height = screen.height;
  bgra->resize(screen.pixels.size());
  for (std::size_t i = 0; i < screen.pixels.size(); ++i)
  {
    const u32 p = screen.pixels[i];
    (*bgra)[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
  }
  return true;
}

void SetView(int port, const View& view, bool place_again)
{
  {
    std::lock_guard lock(s_settings_mutex);
    s_settings.views[port] = view;
  }
  if (place_again)
    s_place_again[port] = true;
  NotifyChanged();
}

void SetShowNumbers(bool show)
{
  {
    std::lock_guard lock(s_settings_mutex);
    s_settings.show_numbers = show;
  }
  NotifyChanged();
}
}  // namespace detail
}  // namespace moderngekko::gba_screen

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
#ifdef HAS_LIBMGBA
  return std::make_unique<moderngekko::gba_screen::Host>(std::move(core));
#else
  return nullptr;
#endif
}
