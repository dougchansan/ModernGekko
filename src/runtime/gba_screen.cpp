#include "gba_screen.hpp"

#include <algorithm>
#include <cstdio>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <imgui.h>

#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/System.h"
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
    screen.fresh = true;
    screen.active = true;
  }

private:
  std::weak_ptr<HW::GBA::Core> m_core;
  int m_port = -1;
};
#endif
}  // namespace

void Draw()
{
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // Twice the GBA's size on a 720p window, scaled with the window; stacked
  // up the right edge, one per GBA.
  const float scale = std::max(1.0f, display.y / 360.0f);
  const float margin = 12.0f;
  float bottom = display.y - margin;
  for (Screen& screen : s_screens)
  {
    {
      std::lock_guard lock(screen.mutex);
      // A new runtime brings a new backend; the old texture died with its own.
      if (screen.texture && screen.texture_gfx != g_gfx.get())
        static_cast<void>(screen.texture.release());
      if (!screen.active)
        continue;
      if (screen.fresh && g_gfx)
      {
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
    }
    if (!screen.texture || !screen.active)
      continue;
    const float w = screen.texture->GetWidth() * scale, h = screen.texture->GetHeight() * scale;
    const ImVec2 a(display.x - margin - w, bottom - h);
    const ImVec2 b(display.x - margin, bottom);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(a.x - 6, a.y - 6), ImVec2(b.x + 6, b.y + 6),
                        IM_COL32(52, 40, 120, 235), 10.0f);
    draw->AddImage(*screen.texture, a, b);
    bottom = a.y - margin - 12.0f;
  }
}
bool ResetGba(int port)
{
#ifdef HAS_LIBMGBA
  if (port < 0 || port >= 4)
    return false;
  std::weak_ptr<HW::GBA::Core> core;
  {
    std::lock_guard lock(s_screens[port].mutex);
    core = s_cores[port];
  }
  if (core.expired())
    return false;
  std::fprintf(stderr, "[gba] port %d: power cycled\n", port + 1);
  ::Core::RunOnCPUThread(::Core::System::GetInstance(), [core] {
    if (const auto running = core.lock())
      running->Reset();
  });
  return true;
#else
  return false;
#endif
}
}  // namespace moderngekko::gba_screen

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
#ifdef HAS_LIBMGBA
  return std::make_unique<moderngekko::gba_screen::Host>(std::move(core));
#else
  return nullptr;
#endif
}
