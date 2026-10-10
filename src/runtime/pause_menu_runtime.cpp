// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/Host.h"
#include "Core/SavestateLayout.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/CameraWarp.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/runtime.hpp"
#include "amd_driver_settings.h"
#include "pause_menu_host.hpp"
#include "pause_menu_policy.hpp"
#include "colosseum_menu_style.hpp"
#include "shadow_tracker.hpp"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <imgui.h>
#include <memory>
#include <mutex>
#include <string>
#include <filesystem>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

bool Host_IsSixtyFpsEnabled();
void Host_ToggleSixtyFps();
bool Host_IsHighResolutionTexturesEnabled();
void Host_ToggleHighResolutionTextures();
bool Host_IsTextUpscaleEnabled();
void Host_ToggleTextUpscale();
bool Host_IsInputOverlayEnabled();
void Host_ToggleInputOverlay();
bool Host_IsAutosaveEnabled();
void Host_ToggleAutosave();
bool Host_IsAudioMuted();
void Host_RequestAudioMuteToggle();
void Host_TogglePauseMenuFullscreen();
void Host_SetExclusiveFullscreen(bool exclusive);
bool Host_IsExclusiveFullscreen();
void Host_SetFastForwardActive(bool active);
void Host_SetPauseMenuResolution(int scale);
void Host_SaveCameraForState(const std::string &state_path);

namespace moderngekko::pause_menu {
namespace {
struct Menu {
  std::mutex mutex;
  std::mutex mods_mutex;
  Policy policy;
  std::array<std::atomic<bool>, 256> keys{};
  std::array<std::atomic<bool>, 256> pressed_keys{};
  std::atomic<bool> active{true}, open{false}, blocked{false}, toggle{false};
  bool select_was_down = false;  // controller Back / Select / Minus edge
  bool run_click_was_down = false;  // left stick click edge
  // Photo mode: world frozen, free camera, A captures, B leaves; D-pad
  // Left / Right field of view, Up / Down depth of field.
  std::atomic<bool> photo_mode{false};
  int photo_fov = 70;
  int photo_dof = 0;
  u16 photo_buttons = 0;
  std::chrono::steady_clock::time_point photo_flash{};
  std::string photo_saved;
  bool running = false;
  // Start pressed without Z: polls it has been held back from the game, so a
  // Z arriving a poll later still makes the menu chord instead of opening the
  // game's own pause screen (-1: Start released to the game until let go).
  int start_held_polls = 0;
  std::atomic<bool> repaint_pending{false};
  ModManager *mods = nullptr;
  std::string config_path;
  // config.ini [Video] backend (the Graphics API used from the next launch).
  std::string graphics_backend = "Vulkan";
  // GPU (GFX.ini [Hardware] Adapter) chosen on the menu row; applied with
  // Enter / A by restarting in place like a graphics API switch.
  int adapter = -1;
  // Graphics API switch in progress: the menu shows a reloading prompt, the
  // game stays paused and input is ignored until the new session takes over.
  std::atomic<bool> switching{false};
  // First-person depth of field: 0 off, 1 very low .. 5 very high (config.ini
  // [Camera] DepthOfField).
  int depth_of_field = 0;
  // First-person horizontal FOV at 16:9 in degrees (0 = game default) and the
  // centre dot (config.ini [Camera] FirstPersonFov / FirstPersonDot).
  int first_person_fov = 100;  // wide enough to play comfortably from the start
  bool first_person_dot = true;
  // First-person turn speed in percent; 0 = Auto (from the frame rate and the
  // field of view, see AutoFirstPersonSensitivity). config.ini [Camera]
  // FirstPersonSensitivity.
  int first_person_sensitivity = 0;
  int applied_auto_sensitivity = -1;  // last Auto value sent to the mod
  // Game speed (config.ini [Gameplay]): text 0 normal .. 3 instant, battle
  // pacing 1..4x, skip the boot logos and movies.
  // Shadow Pokemon page: read from guest memory when the page opens and about
  // once a second while it is shown.
  shadow::Summary shadow;
  std::chrono::steady_clock::time_point shadow_read{};
  int text_speed = 0;
  int battle_speed = 1;
  bool skip_intro = false;
  std::atomic<bool> first_person_active{false};
  // Radeon settings (config.ini [AMD]): -1 leave the driver's value, 0 off,
  // 1 on; and the driver's reported state (-1 unsupported), refreshed when the
  // menu opens or a row changes.
  std::array<int, amd::kFeatureCount> amd_choice{-1, -1, -1, -1, -1, -1};
  std::array<int, amd::kFeatureCount> amd_state{-1, -1, -1, -1, -1, -1};
  // Row last scrolled into view (the list scrolls when it does not fit).
  std::size_t scrolled_focus = SIZE_MAX;
  std::string checkpoint_directory;
  std::string status;
  int sensitivity = 100;
  bool invert_y = false, was_paused = false;
  int adjust_direction = 0;
  double next_adjust = 0;
  bool camera_available = false;
  std::chrono::steady_clock::time_point last_repaint{};
  std::chrono::steady_clock::time_point last_poll{};
  std::chrono::steady_clock::time_point last_look_poll{};
  // Motion test overlay (60 / 120 / 240 Hz lanes) and the camera spin test:
  // a full-right look stick between these steady_clock times (ns).
  std::atomic<bool> motion_test{false};
  std::atomic<long long> spin_start_ns{0}, spin_end_ns{0};
};
std::atomic<std::shared_ptr<Menu>> current;
thread_local bool polling_menu = false;

long long SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool SpinTestActive(const Menu &menu) {
  const long long now = SteadyNs();
  return now >= menu.spin_start_ns.load() && now < menu.spin_end_ns.load();
}

void Filter(int port, GCPadStatus &pad) {
  if (port != 0 || polling_menu)
    return;
  const auto menu = current.load();
  if (!menu || !menu->active)
    return;
  // Camera spin test: the C-stick held fully right.
  if (SpinTestActive(*menu) && !menu->blocked)
    pad.substickX = 255;
  const bool chord = (pad.button & (PAD_BUTTON_START | PAD_TRIGGER_Z)) ==
                     (PAD_BUTTON_START | PAD_TRIGGER_Z);
  // Hold a fresh Start back for a few polls (~50 ms at 60 Hz); if Z does not
  // follow, the game gets Start for as long as it stays held.
  constexpr int kStartGracePolls = 3;
  if (!(pad.button & PAD_BUTTON_START)) {
    menu->start_held_polls = 0;
  } else if (!chord && menu->start_held_polls >= 0) {
    if (++menu->start_held_polls <= kStartGracePolls)
      pad.button &= ~PAD_BUTTON_START;
    else
      menu->start_held_polls = -1;
  } else if (chord) {
    menu->start_held_polls = -1;  // the chord owns this press
  }
  if (menu->blocked || menu->toggle || menu->switching || chord) {
    const bool connected = pad.isConnected;
    pad = GCPadStatus{};
    pad.isConnected = connected;
  }
}

int CurrentFrameRate();
int AutoFirstPersonSensitivity(const Menu &menu);

void Camera(const std::shared_ptr<Menu> &menu, bool recenter = false) {
  const int sensitivity = menu->sensitivity;
  const bool invert = menu->invert_y;
  const int first_person = menu->first_person_sensitivity > 0
                               ? menu->first_person_sensitivity
                               : AutoFirstPersonSensitivity(*menu);
  Core::RunOnCPUThread(
      Core::System::GetInstance(),
      [weak = std::weak_ptr(menu), sensitivity, invert, first_person, recenter] {
        const auto state = weak.lock();
        if (!state)
          return;
        std::lock_guard lock(state->mods_mutex);
        if (!state->active || !state->mods)
          return;
        for (const auto &mod : state->mods->GetLoadedMods()) {
          if (auto function = state->mods->FindExport(
                  mod.id, recenter ? "moderngekko.camera.recenter"
                                   : "moderngekko.camera.configure")) {
            CPUState cpu{};
            cpu.gpr[3] = sensitivity;
            cpu.gpr[4] = invert;
            cpu.gpr[5] = static_cast<u32>(first_person);
            function(&cpu);
            break;
          }
        }
      });
}

// Camera mod state (orbit angle/zoom, first person, free camera) as the ten
// words of moderngekko.camera.get_state (r3..r12), hex-encoded; empty when no
// camera mod is loaded. Carried across the Graphics API switch relaunch.
//
// get_state / set_state only touch the camera mod's own variables (no guest
// memory), so they are called directly: RunOnCPUThread merely queues the job,
// and the game is paused (switch) or not yet running (restore) anyway.
std::string ReadCameraState(const std::shared_ptr<Menu> &menu) {
  std::string encoded;
  std::lock_guard lock(menu->mods_mutex);
  if (!menu->mods)
    return encoded;
  for (const auto &mod : menu->mods->GetLoadedMods()) {
    if (auto function = menu->mods->FindExport(mod.id, "moderngekko.camera.get_state")) {
      CPUState cpu{};
      function(&cpu);
      char buffer[16];
      for (int i = 3; i <= 12; ++i) {
        std::snprintf(buffer, sizeof(buffer), i == 3 ? "%08x" : ",%08x", cpu.gpr[i]);
        encoded += buffer;
      }
      break;
    }
  }
  return encoded;
}

void RestoreCameraState(const std::shared_ptr<Menu> &menu, const std::string &encoded) {
  u32 words[10] = {};
  if (std::sscanf(encoded.c_str(), "%x,%x,%x,%x,%x,%x,%x,%x,%x,%x", &words[0], &words[1],
                  &words[2], &words[3], &words[4], &words[5], &words[6], &words[7], &words[8],
                  &words[9]) != 10)
    return;
  std::lock_guard lock(menu->mods_mutex);
  if (!menu->mods)
    return;
  for (const auto &mod : menu->mods->GetLoadedMods()) {
    if (auto function = menu->mods->FindExport(mod.id, "moderngekko.camera.set_state")) {
      CPUState cpu{};
      for (int i = 0; i < 10; ++i)
        cpu.gpr[3 + i] = words[i];
      function(&cpu);
      std::fprintf(stderr, "[camera] restored (result %u)\n", cpu.gpr[3]);
      break;
    }
  }
}

constexpr std::array<int, 10> kFirstPersonFovs{0, 70, 80, 90, 100, 110, 120, 130, 140, 150};

// Auto first-person turn speed: at full stick each displayed frame turns the
// view by a fixed share of the screen width (0.6%), so motion steps look the
// same size at any frame rate - faster turning at 240 FPS, gentler at 60. The
// width is the real horizontal field of view at the window's aspect ratio
// (ultrawide sees more degrees across the same pixels).
int AutoFirstPersonSensitivity(const Menu &menu) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kStepOfWidth = 0.006;
  constexpr double kGameFovAt169 = 80.0;  // degrees, the game's first-person default
  double aspect = 16.0 / 9.0;
  if (g_presenter && g_presenter->GetBackbufferHeight() > 0)
    aspect = static_cast<double>(g_presenter->GetBackbufferWidth()) /
             g_presenter->GetBackbufferHeight();
  const double fov169 = (menu.first_person_fov > 0 ? menu.first_person_fov : kGameFovAt169) * kPi / 180.0;
  const double hfov = 2.0 * std::atan(std::tan(fov169 * 0.5) * aspect / (16.0 / 9.0));
  const double rad_per_second = kStepOfWidth * CurrentFrameRate() * hfov;
  // The mod turns 2 rad/s at 100%.
  return std::clamp(static_cast<int>(std::lround(rad_per_second / 2.0 * 100.0)), 40, 250);
}

void ApplyFirstPersonFov(const std::shared_ptr<Menu> &menu) {
  std::lock_guard lock(menu->mods_mutex);
  if (!menu->mods)
    return;
  for (const auto &mod : menu->mods->GetLoadedMods()) {
    if (auto function = menu->mods->FindExport(mod.id, "moderngekko.camera.set_fov")) {
      CPUState cpu{};
      cpu.gpr[3] = static_cast<u32>(std::max(menu->first_person_fov, 0));
      function(&cpu);
      std::fprintf(stderr, "[camera] first-person fov %d\n", menu->first_person_fov);
      break;
    }
  }
}

void SaveCameraSetting(const Menu &menu, const char *key, int value) {
  Common::IniFile ini;
  ini.Load(menu.config_path);
  ini.GetOrCreateSection("Camera")->Set(key, value);
  ini.Save(menu.config_path);
}

// Depth of field applies only while the camera mod's first-person view owns the
// camera (not in cutscenes, menus or the free camera).
void UpdateDepthOfField(const std::shared_ptr<Menu> &menu) {
  int level = 0;
  bool first_person = false;
  if (menu->photo_mode) {
    menu->first_person_active = false;
    g_depth_of_field_level.store(menu->photo_dof, std::memory_order_relaxed);
    return;
  }
  if (menu->depth_of_field > 0 || menu->first_person_dot) {
    const std::string state = ReadCameraState(menu);
    const unsigned long flags = state.empty() ? 0 : std::strtoul(state.c_str(), nullptr, 16);
    constexpr unsigned long kFirstPerson = 2, kFree = 4, kActive = 8;
    first_person =
        (flags & (kFirstPerson | kActive)) == (kFirstPerson | kActive) && !(flags & kFree);
    if (first_person)
      level = menu->depth_of_field;
  }
  menu->first_person_active = first_person;
  if (g_depth_of_field_level.exchange(level, std::memory_order_relaxed) != level &&
      std::getenv("MODERNGEKKO_DOF_DEBUG"))
    std::fprintf(stderr, "[dof] level %d (setting %d, first person %d)\n", level,
                 menu->depth_of_field, first_person ? 1 : 0);
}

// Graphics API: stored as config.ini [Video] backend and read by the launcher
// at start-up, so a change applies on the next launch.
#ifdef _WIN32
constexpr std::array<const char *, 2> kGraphicsBackends{"Vulkan", "D3D12"};
#else
constexpr std::array<const char *, 2> kGraphicsBackends{"Vulkan", "OGL"};
#endif

std::string ConfiguredGraphicsBackend(const Menu &menu) {
  Common::IniFile ini;
  ini.Load(menu.config_path);
  std::string backend;
  ini.GetOrCreateSection("Video")->Get("backend", &backend, "Vulkan");
  std::string lower = backend;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "d3d12" || lower == "dx12" || lower == "direct3d12")
    return "D3D12";
  if (lower == "opengl" || lower == "ogl")
    return "OGL";
  return "Vulkan";
}

const char *GraphicsBackendLabel(const std::string &backend) {
  return backend == "D3D12" ? "DirectX 12" : backend == "OGL" ? "OpenGL" : "Vulkan";
}

void CycleGraphicsBackend(const std::shared_ptr<Menu> &menu, int direction) {
  const std::string current = menu->graphics_backend;
  const auto it = std::find(kGraphicsBackends.begin(), kGraphicsBackends.end(), current);
  const int count = static_cast<int>(kGraphicsBackends.size());
  int index = it == kGraphicsBackends.end() ? 0 : static_cast<int>(it - kGraphicsBackends.begin());
  index = (index + (direction < 0 ? -1 : 1) + count) % count;
  // Left / Right only choose; nothing changes until Enter / A confirms.
  const std::string next = kGraphicsBackends[index];
  menu->graphics_backend = next;
  menu->status = next == Config::Get(Config::MAIN_GFX_BACKEND) ?
                     std::string("Graphics API unchanged") :
                     std::string("Press Enter / A to switch to ") + GraphicsBackendLabel(next) +
                         " (the game saves and reloads)";
}

// Enter / A on the Graphics API row: save the choice and switch to it.
// Returns true when a switch to a different API was started or saved.
bool ConfirmGraphicsBackend(const std::shared_ptr<Menu> &menu) {
  const std::string chosen = menu->graphics_backend;
  if (chosen == Config::Get(Config::MAIN_GFX_BACKEND)) {
    menu->status = std::string("Already using ") + GraphicsBackendLabel(chosen);
    return false;
  }
  Common::IniFile ini;
  ini.Load(menu->config_path);
  ini.GetOrCreateSection("Video")->Set("backend", chosen);
  ini.Save(menu->config_path);
#ifdef _WIN32
  menu->status = std::string("Switching to ") + GraphicsBackendLabel(chosen) + "...";
#else
  menu->status = std::string("Restart the game to switch to ") + GraphicsBackendLabel(chosen);
#endif
  return true;
}

void SaveCamera(const std::shared_ptr<Menu> &menu) {
  Common::IniFile ini;
  ini.Load(menu->config_path);
  auto *section = ini.GetOrCreateSection("Camera");
  section->Set("Sensitivity", menu->sensitivity);
  section->Set("InvertY", menu->invert_y);
  ini.Save(menu->config_path);
  Camera(menu);
}

// The controller's Back / Select / Minus / View button. The GameCube pad has no
// such button, so it is read from the host device bound to port 1.
bool SelectButtonDown() {
  if (!Pad::IsInitialized() || !Pad::GetConfig() || !Pad::GetConfig()->GetControllerCount())
    return false;
  ciface::Core::DeviceQualifier qualifier;
  {
    auto lock = ControllerEmu::EmulatedController::GetStateLock();
    qualifier = Pad::GetConfig()->GetController(0)->GetDefaultDevice();
  }
  const auto device = g_controller_interface.FindDevice(qualifier);
  if (!device)
    return false;
  for (const auto *input : device->Inputs()) {
    const std::string name = input->GetName();
    if ((name == "Back" || name == "Select" || name == "Minus" || name == "View" ||
         name == "Share" || name == "Button Back") &&
        input->GetState() > 0.5)
      return true;
  }
  return false;
}

// Left stick click (L3), which the GameCube pad does not have: read from the
// controller device directly, like the Back button above.
bool LeftStickClickDown() {
  if (!Pad::IsInitialized() || !Pad::GetConfig() || !Pad::GetConfig()->GetControllerCount())
    return false;
  ciface::Core::DeviceQualifier qualifier;
  {
    auto lock = ControllerEmu::EmulatedController::GetStateLock();
    qualifier = Pad::GetConfig()->GetController(0)->GetDefaultDevice();
  }
  const auto device = g_controller_interface.FindDevice(qualifier);
  if (!device)
    return false;
  for (const auto *input : device->Inputs()) {
    const std::string name = input->GetName();
    if ((name == "Thumb L" || name == "Left Thumb" || name == "Left Stick" ||
         name == "Left Stick Button" || name == "L3" || name == "Button L3" ||
         name == "Button Thumb L") &&
        input->GetState() > 0.5)
      return true;
  }
  return false;
}

// Photo mode controls, polled with the menu (the camera mod gives the game an
// idle controller meanwhile). Photos are the frame at its internal resolution,
// without the overlay, in User/ScreenShots.
void HandlePhotoMode(const std::shared_ptr<Menu> &menu, const GCPadStatus &pad);

// Game speed in the camera mod (moderngekko.game.configure).
void ConfigureGame(const std::shared_ptr<Menu> &menu) {
  const int text = menu->text_speed, battle = menu->battle_speed;
  const bool skip = menu->skip_intro;
  Core::RunOnCPUThread(Core::System::GetInstance(), [weak = std::weak_ptr(menu), text, battle, skip] {
    const auto state = weak.lock();
    if (!state)
      return;
    std::lock_guard lock(state->mods_mutex);
    if (!state->active || !state->mods)
      return;
    for (const auto &mod : state->mods->GetLoadedMods()) {
      if (auto function = state->mods->FindExport(mod.id, "moderngekko.game.configure")) {
        CPUState cpu{};
        cpu.gpr[3] = static_cast<u32>(text);
        cpu.gpr[4] = static_cast<u32>(battle);
        cpu.gpr[5] = skip ? 1 : 0;
        function(&cpu);
        break;
      }
    }
  });
}

void SaveGameSpeed(const Menu &menu) {
  Common::IniFile ini;
  ini.Load(menu.config_path);
  auto *section = ini.GetOrCreateSection("Gameplay");
  section->Set("TextSpeed", menu.text_speed);
  section->Set("BattleSpeed", menu.battle_speed);
  section->Set("SkipIntro", menu.skip_intro);
  ini.Save(menu.config_path);
}

// Photo mode in the camera mod (moderngekko.camera.set_photo).
void SetPhoto(const std::shared_ptr<Menu> &menu, bool on) {
  const int fov = menu->photo_fov;
  Core::RunOnCPUThread(Core::System::GetInstance(), [weak = std::weak_ptr(menu), on, fov] {
    const auto state = weak.lock();
    if (!state)
      return;
    std::lock_guard lock(state->mods_mutex);
    if (!state->active || !state->mods)
      return;
    for (const auto &mod : state->mods->GetLoadedMods()) {
      if (auto function = state->mods->FindExport(mod.id, "moderngekko.camera.set_photo")) {
        CPUState cpu{};
        cpu.gpr[3] = on ? 1 : 0;
        cpu.gpr[4] = static_cast<u32>(fov);
        function(&cpu);
        break;
      }
    }
  });
}

void LeavePhotoMode(const std::shared_ptr<Menu> &menu);

void HandlePhotoMode(const std::shared_ptr<Menu> &menu, const GCPadStatus &pad) {
  // The menu's input block (held until buttons are released after it closes)
  // would blank the controller the free camera flies with; the camera mod
  // keeps the game itself from seeing it.
  menu->blocked = false;
  u16 buttons = pad.button;
  if (menu->keys[27])
    buttons |= PAD_BUTTON_B;  // Esc leaves too
  if (menu->keys[13])
    buttons |= PAD_BUTTON_A;  // Enter takes a photo
  const u16 pressed = buttons & ~menu->photo_buttons;
  menu->photo_buttons = buttons;
  if (pressed & (PAD_BUTTON_B | PAD_BUTTON_START)) {
    LeavePhotoMode(menu);
    return;
  }
  if (pressed & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
    menu->photo_fov = std::clamp(menu->photo_fov + ((pressed & PAD_BUTTON_RIGHT) ? 10 : -10), 20, 150);
    SetPhoto(menu, true);
  }
  if (pressed & (PAD_BUTTON_UP | PAD_BUTTON_DOWN))
    menu->photo_dof = std::clamp(menu->photo_dof + ((pressed & PAD_BUTTON_UP) ? 1 : -1), 0, 5);
  if (pressed & PAD_BUTTON_A) {
    const std::string directory = File::GetUserPath(D_SCREENSHOTS_IDX);
    File::CreateFullPath(directory);
    const std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof(name), "Colosseum-photo-%Y%m%d-%H%M%S.png", std::localtime(&now));
    const std::string path = directory + name;
    Core::RunOnCPUThread(Core::System::GetInstance(), [path] {
      if (g_frame_dumper)
        g_frame_dumper->SaveScreenshot(path);
    });
    menu->photo_flash = std::chrono::steady_clock::now();
    menu->photo_saved = name;
    std::fprintf(stderr, "[photo] %s\n", path.c_str());
  }
}

FrameDumpResolutionType s_saved_dump_resolution = FrameDumpResolutionType::WindowResolution;

void EnterPhotoResolution() {
  s_saved_dump_resolution = Config::Get(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE);
  Config::SetCurrent(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE,
                     FrameDumpResolutionType::XFBRawResolution);
}

void LeavePhotoMode(const std::shared_ptr<Menu> &menu) {
  menu->photo_mode = false;
  Config::SetCurrent(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE, s_saved_dump_resolution);
  SetPhoto(menu, false);
}

// Run (left stick click toggles): Wes and Rui move and animate twice as fast.
void SetRun(const std::shared_ptr<Menu> &menu, bool running) {
  Core::RunOnCPUThread(Core::System::GetInstance(), [weak = std::weak_ptr(menu), running] {
    const auto state = weak.lock();
    if (!state)
      return;
    std::lock_guard lock(state->mods_mutex);
    if (!state->active || !state->mods)
      return;
    for (const auto &mod : state->mods->GetLoadedMods()) {
      if (auto function = state->mods->FindExport(mod.id, "moderngekko.player.set_run")) {
        CPUState cpu{};
        cpu.gpr[3] = running ? 1 : 0;
        function(&cpu);
        break;
      }
    }
  });
}

std::string Device() {
  if (!Pad::IsInitialized() || !Pad::GetConfig() ||
      !Pad::GetConfig()->GetControllerCount())
    return "No controller";
  auto lock = ControllerEmu::EmulatedController::GetStateLock();
  const auto name =
      Pad::GetConfig()->GetController(0)->GetDefaultDevice().ToString();
  return name.empty() ? "Not selected" : name;
}

void CycleDevice(int direction) {
  auto devices = g_controller_interface.GetAllDeviceStrings();
  std::erase_if(devices, [](const std::string &name) {
    return !name.starts_with("SDL/") && !name.starts_with("XInput/");
  });
  if (devices.empty() || !Pad::IsInitialized() || !Pad::GetConfig() ||
      !Pad::GetConfig()->GetControllerCount())
    return;
  auto *config = Pad::GetConfig();
  auto lock = ControllerEmu::EmulatedController::GetStateLock();
  auto *controller = config->GetController(0);
  const auto found = std::find(devices.begin(), devices.end(),
                               controller->GetDefaultDevice().ToString());
  const int index = found == devices.end()
                        ? (direction > 0 ? -1 : 0)
                        : static_cast<int>(found - devices.begin());
  const int count = static_cast<int>(devices.size());
  controller->SetDefaultDevice(devices[(index + direction + count) % count]);
  controller->UpdateReferences(g_controller_interface);
  config->SaveConfig();
}

// Frame rate setting: 30 (original), 60 (60 FPS mode), 120 / 240 (60 FPS mode
// + frame interpolation with one / three in-between frames per game frame).
std::atomic<bool> s_frame_rate_240{[] {
  const char *env = std::getenv("MODERNGEKKO_FRAME_INTERP");
  return env && env[0] == '4';
}()};

// The requested interpolation state. g_ActiveConfig only catches up on the
// video thread's next config refresh, which does not run while the pause menu
// holds the game, so reading it here would show a stale rate (and make the
// selector skip 60).
bool FrameInterpActive() {
  const int choice = g_frame_interp_choice.load();
  if (choice >= 0)
    return choice == 1;
  return g_ActiveConfig.stereo_mode == StereoMode::FrameInterp;
}

int CurrentFrameRate() {
  if (!Host_IsSixtyFpsEnabled())
    return 30;
  if (!FrameInterpActive())
    return 60;
  return s_frame_rate_240.load() ? 240 : 120;
}

int DisplayRefreshRate();

void SetFrameRate(const std::shared_ptr<Menu> &menu, int rate) {
  // AMD Fluid Motion Frames doubles every presented frame. Above half the
  // display's refresh rate its pacing collapses (multi-second stalls at 240 on
  // a 240 Hz display), so keep frame generation at half the refresh rate and
  // let the driver double it.
  if (rate > 60 && amd::State(amd::Feature::FluidMotionFrames) == 1) {
    const int half = DisplayRefreshRate() / 2;
    const int cap = half >= 240 ? 240 : half >= 120 ? 120 : 60;
    if (rate > cap) {
      rate = cap;
      menu->status = "Fluid Motion Frames is on: game set to " + std::to_string(cap) +
                     " FPS (the driver doubles it to " + std::to_string(cap * 2) + ")";
    }
  }
  if ((rate >= 60) != Host_IsSixtyFpsEnabled())
    Host_ToggleSixtyFps();
  const bool interp = rate >= 120;
  g_frame_interp_choice.store(interp ? 1 : 0);
  // Current layer only: the mode is never saved to GFX.ini.
  Config::SetCurrent(Config::GFX_STEREO_MODE,
                     interp ? StereoMode::FrameInterp : StereoMode::Off);
  s_frame_rate_240.store(rate >= 240);
  g_frame_interp_layers.store(rate >= 240 ? 4 : 2);
  // 120 <-> 240 leaves the stereo mode unchanged, so no config callback would
  // pick up the new layer count; notify explicitly.
  Config::OnConfigChanged();
}

#ifdef _WIN32
// Quotes one argument for CreateProcess / CommandLineToArgvW.
std::wstring QuoteArgument(const std::wstring &arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
    return arg;
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(c);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

// The largest visible top-level window of this process (the render window).
HWND FindSessionWindow() {
  struct Search {
    DWORD pid;
    HWND best;
    LONG area;
  } search{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto *s = reinterpret_cast<Search *>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        RECT r{};
        if (pid == s->pid && IsWindowVisible(hwnd) && GetWindowRect(hwnd, &r)) {
          const LONG area = (r.right - r.left) * (r.bottom - r.top);
          if (area > s->area) {
            s->area = area;
            s->best = hwnd;
          }
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.best;
}

// Saves a state, then relaunches this executable with the same arguments plus
// --graphics <backend> and --load-state <that state>, and stops this session.
// The frame-rate choice and fullscreen carry over through the environment.
void RestartWithGraphicsBackend(const std::shared_ptr<Menu> &menu, const std::string &backend) {
  static std::atomic<bool> restarting{false};
  if (restarting.exchange(true))
    return;
  menu->switching = true;
  const auto state_path =
      std::filesystem::path(menu->checkpoint_directory) / "graphics-switch.sav";
  std::error_code ec;
  std::filesystem::create_directories(state_path.parent_path(), ec);
  std::filesystem::remove(state_path, ec);
  State::SaveAs(Core::System::GetInstance(), state_path.string());

  const int rate = CurrentFrameRate();
  const bool fullscreen = Host_RendererIsFullscreen();
  const std::string camera_state = ReadCameraState(menu);
  std::fprintf(stderr, "[camera] handing over %s\n", camera_state.c_str());
  std::thread([menu, state_path, backend, rate, fullscreen, camera_state] {
    // Back to the menu, still on the current graphics API.
    const auto fail = [&menu] {
      {
        std::lock_guard lock(menu->mutex);
        menu->status = "Couldn't switch the graphics API - still using " +
                       std::string(GraphicsBackendLabel(Config::Get(Config::MAIN_GFX_BACKEND)));
      }
      menu->switching = false;
      restarting.store(false);
    };
    // States are written to a temporary file and renamed when complete.
    for (int i = 0; i < 300 && !std::filesystem::exists(state_path); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!std::filesystem::exists(state_path)) {
      fail();
      return;
    }

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
      fail();
      return;
    }
    std::wstring command_line;
    for (int i = 0; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if ((arg == L"--load-state" || arg == L"--graphics") && i + 1 < argc) {
        ++i;  // replaced below
        continue;
      }
      if (!command_line.empty())
        command_line.push_back(L' ');
      command_line += QuoteArgument(arg);
    }
    LocalFree(argv);
    command_line +=
        L" --graphics " + QuoteArgument(std::wstring(backend.begin(), backend.end()));
    command_line += L" --load-state " + QuoteArgument(state_path.wstring());

    SetEnvironmentVariableW(L"MODERNGEKKO_FRAME_INTERP",
                            rate >= 240 ? L"4" : rate >= 120 ? L"1" : L"0");
    SetEnvironmentVariableW(L"MODERNGEKKO_START_FULLSCREEN", fullscreen ? L"1" : nullptr);
    SetEnvironmentVariableA("MODERNGEKKO_CAMERA_STATE",
                            camera_state.empty() ? nullptr : camera_state.c_str());

    // Seamless handover: the new session opens exactly over this window and
    // signals once it is rendering; only then does this one hide and exit, so
    // the paused frame stays on screen instead of the window disappearing.
    HWND window = FindSessionWindow();
    RECT rect{};
    if (window && GetWindowRect(window, &rect)) {
      const std::wstring value = std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) +
                                 L"," + std::to_wstring(rect.right - rect.left) + L"," +
                                 std::to_wstring(rect.bottom - rect.top);
      SetEnvironmentVariableW(L"MODERNGEKKO_START_RECT", value.c_str());
    }
    const std::string event_name =
        "Local\\ModernGekkoGraphicsSwitch-" + std::to_string(GetCurrentProcessId());
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, event_name.c_str());
    SetEnvironmentVariableA("MODERNGEKKO_SWITCH_EVENT", ready ? event_name.c_str() : nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &startup, &process)) {
      if (ready)
        CloseHandle(ready);
      fail();
      return;
    }
    CloseHandle(process.hThread);
    bool hand_over = true;
    if (ready) {
      // Wait for the new session to render, or for it to fail and exit.
      const HANDLE handles[] = {ready, process.hProcess};
      const DWORD result = WaitForMultipleObjects(2, handles, FALSE, 60000);
      hand_over = result != WAIT_OBJECT_0 + 1;  // keep running if it died
      CloseHandle(ready);
    }
    CloseHandle(process.hProcess);
    if (hand_over)
      amd::HandOver();  // the new session keeps the Radeon overrides
    if (!hand_over) {
      fail();
      return;
    }
    if (window)
      ShowWindow(window, SW_HIDE);
    Host_Message(HostMessageID::WMUserStop);
  }).detach();
}
#endif

void SetRenderScale(const std::shared_ptr<Menu> &menu, int scale) {
  scale = std::clamp(scale, 1, 6);
  Common::IniFile ini;
  ini.Load(menu->config_path);
  ini.GetOrCreateSection("Performance")->Set("ResolutionScale", scale);
  // The ultrawide view keeps its own scale (3..6), used from the next launch.
  ini.GetOrCreateSection("Video")->Set("ultrawide_efb_scale", std::max(scale, 3));
  ini.GetOrCreateSection("Video")->Set("resolution",
                                       std::to_string(640 * scale) + "x" +
                                           std::to_string(528 * scale));
  ini.Save(menu->config_path);
  Core::RunOnCPUThread(Core::System::GetInstance(),
                       [scale] { Host_SetPauseMenuResolution(scale); });
}

void SetPerformanceDisplay(bool show) {
  Config::SetBase(Config::GFX_SHOW_FPS, show);
  Config::SetBase(Config::GFX_SHOW_FTIMES, show);
  Config::SetBase(Config::GFX_SHOW_SPEED, show);
  Config::SetBase(Config::GFX_SHOW_INTERNAL_RESOLUTION, show);
}

// Refresh rate of the monitor showing the game (60 if unknown).
int DisplayRefreshRate() {
#ifdef _WIN32
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  const HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
  if (GetMonitorInfoW(monitor, &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1)
    return static_cast<int>(mode.dmDisplayFrequency);
#endif
  return 60;
}

// Performance presets: one choice sets frame rate, render resolution, VSync
// and the overlays together; any later change to those rows shows "Custom".
struct Preset {
  const char *name;
  int frame_rate;  // 0: the display's refresh rate (up to 240)
  int scale;
  bool vsync;
};
constexpr std::array<Preset, 3> kPresets{{
    // Lowest latency: frames paced by the game, tearing-free on FreeSync /
    // G-Sync displays; modest resolution leaves the most headroom.
    {"Smoothest", 0, 3, false},
    {"Balanced", 120, 4, true},
    {"Quality", 60, 5, true},
}};

int PresetFrameRate(const Preset &preset) {
  if (preset.frame_rate > 0)
    return preset.frame_rate;
  const int refresh = DisplayRefreshRate();
  return refresh >= 240 ? 240 : refresh >= 120 ? 120 : 60;
}

// Index of the preset the current settings match, or -1 (Custom).
int MatchingPreset() {
  if (Config::Get(Config::GFX_SHOW_FPS) || Config::Get(Config::GFX_SHOW_GRAPHS) ||
      Config::Get(Config::GFX_OVERLAY_STATS))
    return -1;
  for (std::size_t i = 0; i < kPresets.size(); ++i) {
    const Preset &preset = kPresets[i];
    if (CurrentFrameRate() == PresetFrameRate(preset) &&
        Config::Get(Config::GFX_EFB_SCALE) == preset.scale &&
        Config::Get(Config::GFX_VSYNC) == preset.vsync)
      return static_cast<int>(i);
  }
  return -1;
}

void ApplyPreset(const std::shared_ptr<Menu> &menu, int direction) {
  const int count = static_cast<int>(kPresets.size());
  const int current = MatchingPreset();
  const int next = current < 0 ? (direction < 0 ? count - 1 : 0)
                               : (current + (direction < 0 ? count - 1 : 1)) % count;
  const Preset &preset = kPresets[static_cast<std::size_t>(next)];
  SetFrameRate(menu, PresetFrameRate(preset));
  if (Config::Get(Config::GFX_EFB_SCALE) != preset.scale)
    SetRenderScale(menu, preset.scale);
  Config::SetBase(Config::GFX_VSYNC, preset.vsync);
  SetPerformanceDisplay(false);
  Config::SetBase(Config::GFX_SHOW_GRAPHS, false);
  Config::SetBase(Config::GFX_OVERLAY_STATS, false);
  Config::Save();
  menu->status.clear();
}

std::string PresetLabel() {
  const int preset = MatchingPreset();
  return std::string("Performance preset   ") +
         (preset < 0 ? "Custom" : kPresets[static_cast<std::size_t>(preset)].name);
}

// Settings rows: 0 is the preset; the rest follow from 1 (Adjust and the
// labels index them from 0 after the preset).
constexpr int kPresetRows = 1;

// 240 Hz camera: the look stick for CameraWarp, read every ~2 ms (the menu's
// own poll runs at 60 Hz). Same dead zone and scale as the camera mod's axis():
// the game's PADStatus C-stick is the raw value less its 128 origin.
float LookAxis(u8 raw) {
  const int v = static_cast<int>(raw) - 128;
  int magnitude = std::min(std::abs(v), 80);
  if (magnitude <= 12)
    return 0.0f;
  return static_cast<float>(magnitude - 12) / 68.0f * (v < 0 ? -1.0f : 1.0f);
}

void PublishLookInput(Menu &menu, std::chrono::steady_clock::time_point now) {
  if (!CameraWarp::IsEnabled() || !Pad::IsInitialized() || menu.policy.is_open() ||
      Core::GetState(Core::System::GetInstance()) != Core::State::Running) {
    CameraWarp::SetLookInput(0, 0);
    return;
  }
  if (now - menu.last_look_poll < std::chrono::milliseconds(2))
    return;
  menu.last_look_poll = now;
  g_controller_interface.UpdateInput();
  polling_menu = true;
  const GCPadStatus pad = Pad::GetStatus(0);
  polling_menu = false;
  if (SpinTestActive(menu))
    CameraWarp::SetLookInput(1.0f, 0.0f);
  else
    CameraWarp::SetLookInput(LookAxis(pad.substickX), LookAxis(pad.substickY));
}

// Dual core row, then the Radeon settings rows in amd::Feature order (row
// numbers after the preset).
constexpr int kAdapterRow = 20;
constexpr int kDualCoreRow = 21;
constexpr int kCamera240Row = 22;
constexpr int kFirstAmdRow = 23;

// The current graphics API's GPUs (index = GFX.ini [Hardware] Adapter).
const std::vector<std::string> &Adapters() { return g_backend_info.Adapters; }

std::string AdapterLabel(const Menu &menu) {
  const auto &adapters = Adapters();
  const int active = Config::Get(Config::GFX_ADAPTER);
  const int chosen = menu.adapter < 0 ? active : menu.adapter;
  std::string name = chosen >= 0 && chosen < static_cast<int>(adapters.size())
                         ? adapters[static_cast<std::size_t>(chosen)]
                         : "GPU " + std::to_string(chosen);
  if (adapters.size() < 2)
    name += "  (only GPU)";
  else if (chosen != active)
    name += "  (Enter / A to apply)";
  return "GPU   " + name;
}
constexpr int kFirstPersonSensitivityRow = kFirstAmdRow + amd::kFeatureCount;
constexpr int kBattleSpeedRow = kFirstPersonSensitivityRow + 1;
constexpr int kTextSpeedRow = kBattleSpeedRow + 1;
constexpr int kMotionTestRow = kTextSpeedRow + 1;
constexpr int kSpinTestRow = kMotionTestRow + 1;
constexpr int kSettingsRows = kPresetRows + kSpinTestRow + 2;  // + Back
constexpr double kSpinTestSeconds = 6.0;

void RefreshAmdStates(Menu &menu) {
  for (int i = 0; i < amd::kFeatureCount; ++i)
    menu.amd_state[i] = amd::State(static_cast<amd::Feature>(i));
}

std::string AmdLabel(const Menu &menu, amd::Feature feature) {
  const int index = static_cast<int>(feature);
  const int state = menu.amd_state[index];
  std::string label = std::string(amd::Name(feature)) + "   ";
  if (state < 0)
    return label + (amd::Available() ? "Not supported" : "Unavailable");
  if (menu.amd_choice[index] < 0)
    return label + "Driver setting (" + (state ? "on" : "off") + ")";
  return label + (state ? "On" : "Off");
}

// Applies a stored choice: -1 gives the setting back to the driver's value.
bool ApplyAmdChoice(amd::Feature feature, int choice) {
  if (choice < 0) {
    amd::Restore(feature);
    return true;
  }
  return amd::Set(feature, choice == 1);
}

void CycleAmdSetting(const std::shared_ptr<Menu> &menu, int index, int direction) {
  const auto feature = static_cast<amd::Feature>(index);
  if (!amd::Available() || menu->amd_state[index] < 0) {
    menu->status = std::string(amd::Name(feature)) +
                   (amd::Available() ? " is not supported on this GPU / driver."
                                     : " needs an AMD Radeon GPU and driver.");
    return;
  }
  // Driver default -> On -> Off -> Driver default (Left goes backwards).
  static constexpr std::array<int, 3> kOrder{-1, 1, 0};
  const auto it = std::find(kOrder.begin(), kOrder.end(), menu->amd_choice[index]);
  const int at = it == kOrder.end() ? 0 : static_cast<int>(it - kOrder.begin());
  const int choice = kOrder[(at + (direction < 0 ? 2 : 1)) % 3];
  if (ApplyAmdChoice(feature, choice)) {
    menu->amd_choice[index] = choice;
    if (feature == amd::Feature::FluidMotionFrames)
      SetFrameRate(menu, CurrentFrameRate());  // cap while it doubles frames
    Common::IniFile ini;
    ini.Load(menu->config_path);
    ini.GetOrCreateSection("AMD")->Set(amd::Key(feature), choice);
    ini.Save(menu->config_path);
    menu->status.clear();
  } else {
    menu->status = std::string("The driver refused ") + amd::Name(feature) +
                   " (Chill, Boost and Anti-Lag can exclude each other).";
  }
  RefreshAmdStates(*menu);
}

// Enter / A on a settings row (Left / Right pass -1 / +1).
constexpr int kAcceptDirection = 2;

void Adjust(const std::shared_ptr<Menu> &menu, int direction) {
  const bool accept = direction == kAcceptDirection;
  if (accept)
    direction = 1;  // other rows treat Enter / A like Right
  if (menu->policy.page() == Page::Controls) {
    CycleDevice(direction);
    return;
  }
  if (menu->policy.focus() < kPresetRows) {
    ApplyPreset(menu, direction);
    return;
  }
  const int row = static_cast<int>(menu->policy.focus()) - kPresetRows;
  if (((row >= 6 && row <= 8) || (row >= 16 && row <= 18)) && !menu->camera_available) {
    menu->status = "Camera controls require the camera mod.";
    return;
  }
  switch (row) {
  case 0: {
    static constexpr std::array<int, 4> kRates{30, 60, 120, 240};
    const auto it = std::find(kRates.begin(), kRates.end(), CurrentFrameRate());
    int index = it == kRates.end() ? 1 : static_cast<int>(it - kRates.begin());
    index = (index + (direction < 0 ? -1 : 1) + static_cast<int>(kRates.size())) %
            static_cast<int>(kRates.size());
    SetFrameRate(menu, kRates[index]);
    break;
  }
  case 1:
    Core::RunOnCPUThread(Core::System::GetInstance(),
                         [] { Host_ToggleHighResolutionTextures(); });
    break;
  case 2:
    Core::RunOnCPUThread(Core::System::GetInstance(),
                         [] { Host_ToggleTextUpscale(); });
    break;
  case 3:
    Host_ToggleInputOverlay();
    break;
  case 4:
    Host_ToggleAutosave();
    break;
  case 5:
    Host_RequestAudioMuteToggle();
    break;
  case 6:
    menu->sensitivity = std::clamp(menu->sensitivity + direction * 10, 50, 200);
    SaveCamera(menu);
    break;
  case 7:
    menu->invert_y = !menu->invert_y;
    SaveCamera(menu);
    break;
  case 8:
    Camera(menu, true);
    break;
  case 9: {
    // Display mode: Windowed, Borderless (fullscreen window) or Exclusive
    // (fullscreen at the monitor's native refresh rate; driver frame
    // generation such as AFMF needs it).
    const int mode = !Host_RendererIsFullscreen() ? 0 : Host_IsExclusiveFullscreen() ? 2 : 1;
    const int next = (mode + (direction < 0 ? 2 : 1)) % 3;
    Host_SetExclusiveFullscreen(next == 2);
    if ((next > 0) != Host_RendererIsFullscreen())
      Host_TogglePauseMenuFullscreen();
    Common::IniFile ini;
    ini.Load(menu->config_path);
    ini.GetOrCreateSection("Video")->Set("ExclusiveFullscreen", next == 2);
    ini.Save(menu->config_path);
    break;
  }
  case 10:
    Config::SetBase(
        Config::MAIN_AUDIO_VOLUME,
        std::clamp(Config::Get(Config::MAIN_AUDIO_VOLUME) + direction * 10, 0,
                   100));
    Config::Save();
    AudioCommon::UpdateSoundStream(Core::System::GetInstance());
    break;
  case 11:
    SetRenderScale(menu, Config::Get(Config::GFX_EFB_SCALE) + direction);
    break;
  case 12:
    Config::SetBase(Config::GFX_VSYNC, !Config::Get(Config::GFX_VSYNC));
    Config::Save();
    break;
  case 13:
    SetPerformanceDisplay(!Config::Get(Config::GFX_SHOW_FPS));
    Config::Save();
    break;
  case 14:
    Config::SetBase(Config::GFX_SHOW_GRAPHS,
                    !Config::Get(Config::GFX_SHOW_GRAPHS));
    Config::Save();
    break;
  case 15:
    Config::SetBase(Config::GFX_OVERLAY_STATS,
                    !Config::Get(Config::GFX_OVERLAY_STATS));
    Config::Save();
    break;
  case 16: {
    menu->depth_of_field = (menu->depth_of_field + (direction < 0 ? 5 : 1)) % 6;
    Common::IniFile ini;
    ini.Load(menu->config_path);
    ini.GetOrCreateSection("Camera")->Set("DepthOfField", menu->depth_of_field);
    ini.Save(menu->config_path);
    break;
  }
  case 17: {
    const auto it = std::find(kFirstPersonFovs.begin(), kFirstPersonFovs.end(), menu->first_person_fov);
    const int count = static_cast<int>(kFirstPersonFovs.size());
    int index = it == kFirstPersonFovs.end() ? 0 : static_cast<int>(it - kFirstPersonFovs.begin());
    index = (index + (direction < 0 ? -1 : 1) + count) % count;
    menu->first_person_fov = kFirstPersonFovs[index];
    SaveCameraSetting(*menu, "FirstPersonFov", menu->first_person_fov);
    ApplyFirstPersonFov(menu);
    break;
  }
  case 18:
    menu->first_person_dot = !menu->first_person_dot;
    SaveCameraSetting(*menu, "FirstPersonDot", menu->first_person_dot ? 1 : 0);
    break;
  case 19:
    if (!accept) {
      CycleGraphicsBackend(menu, direction);
    } else if (ConfirmGraphicsBackend(menu)) {
#ifdef _WIN32
      RestartWithGraphicsBackend(menu, menu->graphics_backend);
#endif
    }
    break;
  case kAdapterRow: {
    const int count = static_cast<int>(Adapters().size());
    if (count < 2)
      break;
    const int active = Config::Get(Config::GFX_ADAPTER);
    if (!accept) {
      const int chosen = menu->adapter < 0 ? active : menu->adapter;
      menu->adapter = ((chosen + (direction < 0 ? -1 : 1)) % count + count) % count;
    } else if (menu->adapter >= 0 && menu->adapter != active) {
      // Read at video backend start: save it and restart in place.
      Config::SetBase(Config::GFX_ADAPTER, menu->adapter);
      Config::Save();
#ifdef _WIN32
      RestartWithGraphicsBackend(menu, Config::Get(Config::MAIN_GFX_BACKEND));
#endif
    }
    break;
  }
  case kDualCoreRow: {
    // Dolphin reads it at boot: save it and restart in place like a graphics
    // API switch (save state, relaunch, reload).
    Common::IniFile ini;
    ini.Load(menu->config_path);
    bool dual_core = true;
    ini.GetOrCreateSection("QualityOfLife")->Get("dual_core", &dual_core, true);
    ini.GetOrCreateSection("QualityOfLife")->Set("dual_core", !dual_core);
    ini.Save(menu->config_path);
#ifdef _WIN32
    RestartWithGraphicsBackend(menu, Config::Get(Config::MAIN_GFX_BACKEND));
#endif
    break;
  }
  case kCamera240Row: {
    // 240 Hz camera (CameraWarp): first-person / free camera turns follow the
    // look stick at the display rate instead of the 60 Hz game frames.
    const bool on = !CameraWarp::IsEnabled();
    CameraWarp::SetEnabled(on);
    SaveCameraSetting(*menu, "Camera240", on ? 1 : 0);
    break;
  }
  case kFirstAmdRow + 0:
  case kFirstAmdRow + 1:
  case kFirstAmdRow + 2:
  case kFirstAmdRow + 3:
  case kFirstAmdRow + 4:
  case kFirstAmdRow + 5:
    CycleAmdSetting(menu, row - kFirstAmdRow, direction);
    break;
  case kFirstPersonSensitivityRow: {
    // Auto, then 40% .. 250% in steps of 10.
    int value = menu->first_person_sensitivity;
    if (value <= 0)
      value = direction > 0 ? 40 : 250;
    else
      value += direction * 10;
    menu->first_person_sensitivity = value < 40 || value > 250 ? 0 : value;
    SaveCameraSetting(*menu, "FirstPersonSensitivity", menu->first_person_sensitivity);
    Camera(menu);
    break;
  }
  case kTextSpeedRow:
    menu->text_speed = (menu->text_speed + (direction < 0 ? 3 : 1)) % 4;
    SaveGameSpeed(*menu);
    ConfigureGame(menu);
    break;
  case kBattleSpeedRow:
    menu->battle_speed = (menu->battle_speed - 1 + (direction < 0 ? 3 : 1)) % 4 + 1;
    SaveGameSpeed(*menu);
    ConfigureGame(menu);
    break;
  case kMotionTestRow:
    // Closes the menu so the lanes animate at the game's present rate.
    menu->motion_test = !menu->motion_test;
    if (menu->motion_test)
      menu->policy.close();
    break;
  case kSpinTestRow: {
    const long long start = SteadyNs() + 500'000'000;  // after the menu closes
    menu->spin_start_ns = start;
    menu->spin_end_ns = start + static_cast<long long>(kSpinTestSeconds * 1e9);
    menu->policy.close();
    break;
  }
  default:
    break;
  }
}

void ActionHost(const std::shared_ptr<Menu> &menu, Action action,
                [[maybe_unused]] int direction = 1) {
  switch (action) {
  case Action::Shadow:
    menu->shadow = shadow::Read();
    menu->shadow_read = std::chrono::steady_clock::now();
    break;
  case Action::PhotoMode:
    if (!menu->camera_available) {
      menu->status = "Photo mode requires the camera mod.";
      break;
    }
    menu->photo_mode = true;
    menu->photo_buttons = 0xFFFF;  // ignore the A that chose it
    EnterPhotoResolution();
    menu->photo_saved.clear();
    SetPhoto(menu, true);
    break;
  case Action::SaveCheckpoint: {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto path =
        std::filesystem::path(menu->checkpoint_directory) /
        State::Layout::TimestampedName(static_cast<std::time_t>(stamp / 1000),
                                       static_cast<std::size_t>(stamp % 1000),
                                       State::Layout::MANUAL_PREFIX);
    if (File::CreateFullPath(path.string())) {
      State::SaveAs(Core::System::GetInstance(), path.string());
      Host_SaveCameraForState(path.string());
      menu->status = "Checkpoint save requested";
    } else
      menu->status = "Unable to create checkpoint folder";
    break;
  }
  case Action::Restart:
    // What the console's RESET button does: back to the title sequence.
    Core::RunOnCPUThread(Core::System::GetInstance(), [] {
      Core::System::GetInstance().GetProcessorInterface().ResetButton_Tap();
    });
    break;
  case Action::Quit:
    Host_Message(HostMessageID::WMUserStop);
    break;
  case Action::AdjustSetting:
    // Only Enter / A reaches here; Left / Right call Adjust directly.
    Adjust(menu, kAcceptDirection);
    break;
  default:
    break;
  }
}

void SyncPause(const std::shared_ptr<Menu> &menu, bool previously_open) {
  const bool open = menu->policy.is_open();
  auto &system = Core::System::GetInstance();
  if (open && !previously_open) {
    Host_SetFastForwardActive(false);
    RefreshAmdStates(*menu);  // the user may have changed them in Adrenalin
    menu->was_paused = Core::GetState(system) == Core::State::Paused;
    if (Core::GetState(system) == Core::State::Running)
      Core::SetState(system, Core::State::Paused);
  } else if (!open && previously_open && !menu->was_paused &&
             Core::GetState(system) == Core::State::Paused) {
    Core::SetState(system, Core::State::Running);
  }
  menu->open = open;
  menu->blocked = menu->policy.blocks_game_input();
}

std::vector<std::string> Rows(const Menu &menu) {
  auto enabled = [](const char *title, bool value) {
    return std::string(title) + (value ? "   ON" : "   OFF");
  };
  switch (menu.policy.page()) {
  case Page::Home:
    return {"Resume adventure", "Settings", "Controls", "Shadow Pok\xC3\xA9mon",
            "Photo mode", "Save checkpoint", "Restart game", "Quit game"};
  case Page::Shadow: {
    std::vector<std::string> rows;
    for (const auto &entry : menu.shadow.entries) {
      const char *status = entry.status == shadow::Status::Purified ? "PURIFIED"
                           : entry.status == shadow::Status::Snagged ? "SNAGGED"
                           : entry.status == shadow::Status::Seen    ? "SEEN"
                                                                     : "NOT SEEN";
      rows.push_back(entry.name + "   " + status);
    }
    rows.push_back("Back");
    return rows;
  }
  case Page::Settings:
    return {
        PresetLabel(),
        "Frame rate   " + std::to_string(CurrentFrameRate()) + " FPS" +
            (CurrentFrameRate() >= 120 && g_frame_interp_dropping.load()
                 ? "  (display can't keep up - showing 60)"
                 : ""),
        enabled("HD textures", Host_IsHighResolutionTexturesEnabled()),
        enabled("Sharper text", Host_IsTextUpscaleEnabled()),
        enabled("Input overlay", Host_IsInputOverlayEnabled()),
        enabled("Autosave", Host_IsAutosaveEnabled()),
        enabled("Mute audio", Host_IsAudioMuted()),
        "Camera sensitivity   " + std::to_string(menu.sensitivity) + "%",
        enabled("Invert camera Y", menu.invert_y),
        "Recenter camera",
        std::string("Display mode   ") +
            (!Host_RendererIsFullscreen()    ? "Windowed"
             : Host_IsExclusiveFullscreen() ? "Exclusive fullscreen"
                                            : "Borderless fullscreen"),
        "Audio volume   " +
            std::to_string(Config::Get(Config::MAIN_AUDIO_VOLUME)) + "%",
        "Render resolution   " +
            (Config::Get(Config::GFX_EFB_SCALE) == 0
                 ? std::string("Auto")
                 : std::to_string(Config::Get(Config::GFX_EFB_SCALE)) +
                       "x native"),
        enabled("VSync", Config::Get(Config::GFX_VSYNC)),
        enabled("Performance display", Config::Get(Config::GFX_SHOW_FPS)),
        enabled("Frame-time graph", Config::Get(Config::GFX_SHOW_GRAPHS)),
        enabled("Render statistics", Config::Get(Config::GFX_OVERLAY_STATS)),
        std::string("First-person depth of field   ") +
            std::array<const char *, 6>{"Off", "Very low", "Low", "Medium", "High",
                                        "Very high"}[static_cast<std::size_t>(
                std::clamp(menu.depth_of_field, 0, 5))],
        std::string("First-person field of view   ") +
            (menu.first_person_fov > 0 ? std::to_string(menu.first_person_fov) + " deg"
                                       : std::string("Game default")),
        enabled("First-person centre dot", menu.first_person_dot),
        [&menu] {
          const std::string &configured = menu.graphics_backend;
          return std::string("Graphics API   ") + GraphicsBackendLabel(configured) +
                 (configured != Config::Get(Config::MAIN_GFX_BACKEND) ? "  (Enter / A to apply)"
                                                                       : "");
        }(),
        AdapterLabel(menu),
        enabled("Dual core (CPU + GPU threads)", Config::Get(Config::MAIN_CPU_THREAD)),
        enabled("240 Hz camera", CameraWarp::IsEnabled()),
        AmdLabel(menu, amd::Feature::AntiLag),
        AmdLabel(menu, amd::Feature::Chill),
        AmdLabel(menu, amd::Feature::Boost),
        AmdLabel(menu, amd::Feature::ImageSharpening),
        AmdLabel(menu, amd::Feature::EnhancedSync),
        AmdLabel(menu, amd::Feature::FluidMotionFrames),
        "First-person turn speed   " +
            (menu.first_person_sensitivity > 0
                 ? std::to_string(menu.first_person_sensitivity) + "%"
                 : "Auto (" + std::to_string(AutoFirstPersonSensitivity(menu)) +
                       "% at " + std::to_string(CurrentFrameRate()) + " FPS)"),
        "Battle speed   " + std::to_string(menu.battle_speed) + "x",
        std::string("Text speed   ") +
            std::array<const char *, 4>{"Normal", "Fast", "Faster", "Instant"}[static_cast<std::size_t>(
                std::clamp(menu.text_speed, 0, 3))],
        enabled("Motion test (60 / 120 / 240 Hz lanes)", menu.motion_test),
        "Camera spin test (6 s, C-stick held right)",
        "Back"};
  case Page::Controls: {
    std::string name = Device();
    const auto first = name.find('/');
    const auto second =
        first == std::string::npos ? first : name.find('/', first + 1);
    if (second != std::string::npos)
      name = name.substr(second + 1);
    return {"Controller   " + name, "Back"};
  }
  case Page::QuitConfirm:
    return {"Keep playing", "Quit game"};
  case Page::RestartConfirm:
    return {"Keep playing", "Restart game"};
  default:
    return {};
  }
}
} // namespace

void Initialize(const RuntimeConfig &config, ModManager *mods) {
  Shutdown();
  auto menu = std::make_shared<Menu>();
  menu->mods = mods;
  if (mods)
    for (const auto &mod : mods->GetLoadedMods())
      if (mods->FindExport(mod.id, "moderngekko.camera.configure"))
        menu->camera_available = true;
  menu->config_path = (config.user_directory / "config.ini").string();
  menu->graphics_backend = ConfiguredGraphicsBackend(*menu);
  menu->checkpoint_directory =
      ((config.automation.directory.empty() ? config.user_directory
                                            : config.automation.directory) /
       "states")
          .string();
  Common::IniFile ini;
  ini.Load(menu->config_path);
  const auto *section = ini.GetOrCreateSection("Camera");
  section->Get("Sensitivity", &menu->sensitivity, 100);
  section->Get("InvertY", &menu->invert_y, false);
  section->Get("DepthOfField", &menu->depth_of_field, 0);
  menu->depth_of_field = std::clamp(menu->depth_of_field, 0, 5);
  section->Get("FirstPersonFov", &menu->first_person_fov, 100);
  section->Get("FirstPersonDot", &menu->first_person_dot, true);
  section->Get("FirstPersonSensitivity", &menu->first_person_sensitivity, 0);
  {
    auto *gameplay = ini.GetOrCreateSection("Gameplay");
    gameplay->Get("TextSpeed", &menu->text_speed, 0);
    gameplay->Get("BattleSpeed", &menu->battle_speed, 1);
    gameplay->Get("SkipIntro", &menu->skip_intro, false);
    menu->text_speed = std::clamp(menu->text_speed, 0, 3);
    menu->battle_speed = std::clamp(menu->battle_speed, 1, 4);
  }
  if (menu->first_person_sensitivity != 0)
    menu->first_person_sensitivity = std::clamp(menu->first_person_sensitivity, 40, 250);
  {
    bool camera240 = true;
    section->Get("Camera240", &camera240, true);
    const char *env = std::getenv("MODERNGEKKO_CAMERA_WARP");
    CameraWarp::SetEnabled(camera240 && !(env && env[0] == '0'));
  }
  {
    // Radeon settings: restore values a crashed session left changed (or take
    // over from the previous session during a graphics API switch), then
    // apply this game's choices.
    const char *switch_event = std::getenv("MODERNGEKKO_SWITCH_EVENT");
    amd::Initialize(config.user_directory / "amd-driver-restore.ini",
                    switch_event && *switch_event);
    const auto *amd_section = ini.GetOrCreateSection("AMD");
    for (int i = 0; i < amd::kFeatureCount; ++i) {
      amd_section->Get(amd::Key(static_cast<amd::Feature>(i)), &menu->amd_choice[i], -1);
      menu->amd_choice[i] = std::clamp(menu->amd_choice[i], -1, 1);
      if (menu->amd_choice[i] >= 0 &&
          !ApplyAmdChoice(static_cast<amd::Feature>(i), menu->amd_choice[i]))
        std::fprintf(stderr, "[amd] %s could not be applied\n",
                     amd::Name(static_cast<amd::Feature>(i)));
    }
    RefreshAmdStates(*menu);
    if (menu->amd_state[static_cast<int>(amd::Feature::FluidMotionFrames)] == 1)
      SetFrameRate(menu, CurrentFrameRate());
    for (int i = 0; i < amd::kFeatureCount; ++i)
      std::fprintf(stderr, "[amd] %s: %s\n", amd::Name(static_cast<amd::Feature>(i)),
                   menu->amd_state[i] < 0 ? "unsupported" : menu->amd_state[i] ? "on" : "off");
  }
  bool exclusive_fullscreen = false;
  ini.GetOrCreateSection("Video")->Get("ExclusiveFullscreen", &exclusive_fullscreen, false);
  Host_SetExclusiveFullscreen(exclusive_fullscreen);
  menu->sensitivity = std::clamp(menu->sensitivity, 50, 200);
  current.store(menu);
#ifdef _WIN32
  if (!config.headless)
    Pad::SetStatusFilter(&Filter);
#endif
  Camera(menu);
  ConfigureGame(menu);
  ApplyFirstPersonFov(menu);
  // MODERNGEKKO_CAMERA_STATE: camera angle handed over by the Graphics API
  // switch; restored before the first field update.
  if (const char *camera_state = std::getenv("MODERNGEKKO_CAMERA_STATE");
      camera_state && *camera_state) {
    RestoreCameraState(menu, camera_state);
#ifdef _WIN32
    SetEnvironmentVariableA("MODERNGEKKO_CAMERA_STATE", nullptr);
#endif
  } else if (config.load_state_path) {
    // Started from a save state: its camera, if one was saved with it.
    std::ifstream in(config.load_state_path->string() + ".camera");
    std::string encoded;
    if (in && std::getline(in, encoded) && !encoded.empty())
      RestoreCameraState(menu, encoded);
  }
#ifdef _WIN32
  // MODERNGEKKO_TEST_GRAPHICS_SWITCH=<Vulkan|D3D12>: test hook that performs
  // the menu's save-restart-reload switch 8 s after start (cleared first so the
  // relaunched session does not repeat it).
  if (const char *test = std::getenv("MODERNGEKKO_TEST_GRAPHICS_SWITCH"); test && *test) {
    // Optional ":<seconds>" delay suffix (default 8).
    std::string target = test;
    int delay = 8;
    if (const auto colon = target.find(':'); colon != std::string::npos) {
      delay = std::atoi(target.c_str() + colon + 1);
      target.resize(colon);
    }
    SetEnvironmentVariableW(L"MODERNGEKKO_TEST_GRAPHICS_SWITCH", nullptr);
    std::thread([menu, target, delay] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      menu->graphics_backend = target;
      RestartWithGraphicsBackend(menu, target);
    }).detach();
  }
#endif
  // MODERNGEKKO_TEST_FRAME_RATE=<30|60|120|240>: test hook applying the menu's
  // frame-rate setting 8 s after start.
  if (const char *test = std::getenv("MODERNGEKKO_TEST_FRAME_RATE"); test && *test) {
    const int rate = std::atoi(test);
    std::thread([menu, rate] {
      std::this_thread::sleep_for(std::chrono::seconds(8));
      std::lock_guard lock(menu->mutex);
      SetFrameRate(menu, rate);
    }).detach();
  }
  // MODERNGEKKO_TEST_AMD_ROW=<0-5>: test hook cycling that Radeon setting (as
  // Right on its menu row) 12 s after start.
  if (const char *test = std::getenv("MODERNGEKKO_TEST_AMD_ROW"); test && *test) {
    const int index = std::clamp(std::atoi(test), 0, amd::kFeatureCount - 1);
    std::thread([menu, index] {
      std::this_thread::sleep_for(std::chrono::seconds(12));
      std::lock_guard lock(menu->mutex);
      CycleAmdSetting(menu, index, 1);
    }).detach();
  }
  // MODERNGEKKO_TEST_MOTION=1: motion test overlay on from the start;
  // MODERNGEKKO_TEST_SPIN=<seconds>: start the camera spin test after a delay.
  if (const char *test = std::getenv("MODERNGEKKO_TEST_MOTION"); test && *test == '1')
    menu->motion_test = true;
  if (const char *test = std::getenv("MODERNGEKKO_TEST_SPIN"); test && *test) {
    const long long start = SteadyNs() + static_cast<long long>(std::atoi(test)) * 1'000'000'000;
    menu->spin_start_ns = start;
    menu->spin_end_ns = start + static_cast<long long>(kSpinTestSeconds * 1e9);
  }
  // MODERNGEKKO_TEST_MENU=<home|settings|controls|quit>[:seconds]: opens the
  // menu on that page after a delay (default 10 s) for UI screenshots.
  if (const char *test = std::getenv("MODERNGEKKO_TEST_MENU"); test && *test) {
    std::string target = test;
    int delay = 10;
    if (const auto colon = target.find(':'); colon != std::string::npos) {
      delay = std::atoi(target.c_str() + colon + 1);
      target.resize(colon);
    }
    std::thread([menu, target, delay] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      std::lock_guard lock(menu->mutex);
      const bool was_open = menu->policy.is_open();
      menu->policy.open();
      if (target == "settings")
        menu->policy.activate(1);
      else if (target == "controls")
        menu->policy.activate(2);
      else if (target == "quit")
        menu->policy.activate(7);
      else if (target == "photo")
        ActionHost(menu, menu->policy.activate(4));
      else if (target == "shadow")
        ActionHost(menu, menu->policy.activate(3));
      SyncPause(menu, was_open);
    }).detach();
  }
  // MODERNGEKKO_TEST_RUN=1: start with running on (as a left stick click).
  if (const char *test = std::getenv("MODERNGEKKO_TEST_RUN"); test && *test == '1') {
    menu->running = true;
    SetRun(menu, true);
  }
  // MODERNGEKKO_TEST_RESTART=<seconds>: test hook choosing Restart game.
  if (const char *test = std::getenv("MODERNGEKKO_TEST_RESTART"); test && *test) {
    const int delay = std::atoi(test);
    std::thread([menu, delay] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      std::lock_guard lock(menu->mutex);
      ActionHost(menu, Action::Restart);
    }).detach();
  }
  int resolution = 0;
  ini.GetOrCreateSection("Performance")->Get("ResolutionScale", &resolution, 0);
  if (resolution >= 1 && resolution <= 6)
    Core::RunOnCPUThread(Core::System::GetInstance(), [resolution] {
      Host_SetPauseMenuResolution(resolution);
    });
}

void Shutdown() {
  amd::Shutdown();
  auto menu = current.exchange(nullptr);
  Pad::SetStatusFilter(nullptr);
  if (!menu)
    return;
  std::lock_guard lock(menu->mutex);
  std::lock_guard mods_lock(menu->mods_mutex);
  menu->active = false;
  menu->mods = nullptr;
}

void DrawSwitchingPrompt(const Menu &menu) {
  const auto &io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                IM_COL32(3, 10, 27, 200));
  const float scale = std::clamp(io.DisplaySize.y / 900.0f, 0.65f, 1.5f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.065f, 0.17f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.80f, 0.18f, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32 * scale, 24 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2);
  ImGui::Begin("Colosseum graphics switch", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
  ImGui::SetWindowFontScale(scale * 1.5f);
  ImGui::TextColored(ImVec4(1, 0.83f, 0.25f, 1), "Reloading graphics backend...");
  ImGui::Text("Switching to %s - your game will continue right here.",
              GraphicsBackendLabel(menu.graphics_backend));
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

// First-person centre dot: where the view (and depth-of-field focus) points.
void DrawCentreDot() {
  const auto &io = ImGui::GetIO();
  const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  const float radius = std::max(2.0f, io.DisplaySize.y / 420.0f);
  auto *draw = ImGui::GetForegroundDrawList();
  draw->AddCircleFilled(centre, radius + 1.5f, IM_COL32(0, 0, 0, 150), 16);
  draw->AddCircleFilled(centre, radius, IM_COL32(255, 255, 255, 220), 16);
}

// Motion test: three boxes crossing the screen at the same speed, each moved
// only 60, 120 or 240 times a second. On a display (and frame rate) that fast,
// the lower lanes visibly step and smear; the measured present rate says what
// the screen is actually getting.
void DrawMotionTest() {
  const auto &io = ImGui::GetIO();
  static double last = 0, rate = 0;
  const double t = SteadyNs() * 1e-9;
  if (last > 0 && t > last && t - last < 0.25)
    rate = rate <= 0 ? 1.0 / (t - last) : rate * 0.95 + 0.05 / (t - last);
  last = t;
  auto *draw = ImGui::GetForegroundDrawList();
  const float scale = std::clamp(io.DisplaySize.y / 900.0f, 0.65f, 1.5f);
  const float lane = 70 * scale, box = 46 * scale, margin = 24 * scale;
  const float top = io.DisplaySize.y * 0.5f - lane * 1.5f - 20 * scale;
  const float width = io.DisplaySize.x - 2 * margin;
  draw->AddRectFilled(ImVec2(0, top - 44 * scale), ImVec2(io.DisplaySize.x, top + lane * 3 + 8 * scale),
                      IM_COL32(0, 0, 0, 215));
  char text[160];
  std::snprintf(text, sizeof(text),
                "MOTION TEST   display %d Hz   presenting %.0f fps   frame rate setting %d   "
                "(Settings > Motion test to close)",
                DisplayRefreshRate(), rate, CurrentFrameRate());
  draw->AddText(nullptr, 22 * scale, ImVec2(margin, top - 38 * scale), IM_COL32(255, 214, 64, 255), text);
  static constexpr std::array<int, 3> kRates{60, 120, 240};
  const double speed = width / 1.5;  // one crossing every 1.5 s
  for (std::size_t i = 0; i < kRates.size(); ++i) {
    const double step = std::floor(t * kRates[i]) / kRates[i];
    const float x = margin + static_cast<float>(std::fmod(step * speed, width - box));
    const float y = top + lane * static_cast<float>(i);
    draw->AddRectFilled(ImVec2(margin, y + lane - 3 * scale), ImVec2(margin + width, y + lane - 2 * scale),
                        IM_COL32(255, 255, 255, 60));
    draw->AddRectFilled(ImVec2(x, y + (lane - box) * 0.5f), ImVec2(x + box, y + (lane + box) * 0.5f),
                        IM_COL32(240, 240, 255, 255));
    std::snprintf(text, sizeof(text), "%d Hz%s", kRates[i],
                  rate + 5 < kRates[i] ? "  (above the present rate: looks like the lanes below)" : "");
    draw->AddText(nullptr, 20 * scale, ImVec2(margin + 6 * scale, y + 4 * scale), IM_COL32(120, 220, 255, 255),
                  text);
  }
}

void DrawSpinTest(const Menu &menu) {
  const long long now = SteadyNs();
  const long long end = menu.spin_end_ns.load();
  if (now >= end || now < menu.spin_start_ns.load() - 1'000'000'000)
    return;
  const auto &io = ImGui::GetIO();
  const float scale = std::clamp(io.DisplaySize.y / 900.0f, 0.65f, 1.5f);
  char text[128];
  std::snprintf(text, sizeof(text), "CAMERA SPIN TEST   %.1f s left   frame rate %d   240 Hz camera %s",
                (end - now) * 1e-9, CurrentFrameRate(), CameraWarp::IsEnabled() ? "on" : "off");
  ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, 40 * scale),
                                                IM_COL32(0, 0, 0, 190));
  ImGui::GetForegroundDrawList()->AddText(nullptr, 24 * scale, ImVec2(16 * scale, 8 * scale),
                                          IM_COL32(255, 214, 64, 255), text);
}

// The menu font: Colosseum's own (MenuFont() in colosseum_font) or ImGui's.
ImFont *MenuFont();

// Settings rows grouped like the game's own option screens.
const char *SettingsSection(std::size_t row) {
  switch (row) {
  case 0: return "PERFORMANCE";
  case 1: return "GAME";
  case 7: return "CAMERA";
  case 10: return "DISPLAY & SOUND";
  case 17: return "FIRST PERSON";
  case 20: return "GRAPHICS";
  case static_cast<std::size_t>(kPresetRows + kFirstAmdRow): return "AMD RADEON";
  case static_cast<std::size_t>(kPresetRows + kFirstPersonSensitivityRow): return "FEEL";
  case static_cast<std::size_t>(kPresetRows + kBattleSpeedRow): return "BATTLE SPEED";
  case static_cast<std::size_t>(kPresetRows + kMotionTestRow): return "TESTS";
  default: return nullptr;
  }
}

// "Label   value" rows (three spaces) split into the label and its value.
std::pair<std::string, std::string> SplitRow(const std::string &row) {
  const auto at = row.find("   ");
  if (at == std::string::npos)
    return {row, {}};
  std::string value = row.substr(at);
  value.erase(0, value.find_first_not_of(' '));
  return {row.substr(0, at), value};
}

void QueueActivate(const std::shared_ptr<Menu> &menu, std::size_t row) {
  Core::QueueHostJob([weak = std::weak_ptr(menu), row](Core::System &) {
    auto state = weak.lock();
    if (!state)
      return;
    std::lock_guard guard(state->mutex);
    if (!state->active)
      return;
    const bool open = state->policy.is_open();
    ActionHost(state, state->policy.activate(row));
    SyncPause(state, open);
  });
}

std::string Upper(std::string text) {
  for (char &ch : text)
    if (ch >= 'a' && ch <= 'z')
      ch = static_cast<char>(ch - 'a' + 'A');
  return text;
}

// Photo mode: a hint bar at the bottom, a flash and the saved file name after a
// photo. None of it is in the photo (it is taken from the game's frame).
void DrawPhotoMode(Menu &menu) {
  namespace st = style;
  namespace col = style::color;
  const auto &io = ImGui::GetIO();
  ImFont *font = MenuFont();
  const float s = std::clamp(io.DisplaySize.y / 900.0f, 0.65f, 1.6f);
  ImDrawList *draw = ImGui::GetForegroundDrawList();
  const auto now = std::chrono::steady_clock::now();
  const float since = std::chrono::duration<float>(now - menu.photo_flash).count();
  if (since < 0.25f)
    draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                        st::Rgb(255, 255, 255, static_cast<int>(200 * (1 - since / 0.25f))));
  const float width = std::min(1100 * s, io.DisplaySize.x - 32 * s), height = 104 * s;
  const ImVec2 a((io.DisplaySize.x - width) * 0.5f, io.DisplaySize.y - height - 24 * s);
  const ImVec2 b(a.x + width, a.y + height);
  st::ChromePlate(draw, a, b, 16 * s, 7 * s, st::Rgb(20, 70, 64, 235), st::Rgb(10, 40, 38, 235));
  const float size = 24 * s;
  st::FillChamfer(draw, ImVec2(a.x + 18 * s, a.y + 18 * s), ImVec2(a.x + 26 * s, b.y - 18 * s), 3 * s,
                  st::Rgb(240, 70, 60), st::Rgb(150, 20, 20));
  st::OutlinedText(draw, font, size * 1.2f, ImVec2(a.x + 40 * s, a.y + 16 * s), col::kFocusGreen,
                   "PHOTO MODE", 1.8f * s);
  static const char *const kDof[] = {"Off", "Very low", "Low", "Medium", "High", "Very high"};
  char status[160];
  std::snprintf(status, sizeof(status), "Field of view %d    Depth of field %s", menu.photo_fov,
                kDof[std::clamp(menu.photo_dof, 0, 5)]);
  st::OutlinedText(draw, font, size, ImVec2(a.x + 260 * s, a.y + 20 * s), col::kYellow, status, 1.5f * s);
  const std::string hints = since < 3.0f && !menu.photo_saved.empty()
                                ? "Saved " + menu.photo_saved
                                : "A: take photo   B: back   Sticks: fly / look   R / L: up / down   "
                                  "D-pad: view / focus";
  st::SpacedText(draw, font, size * 0.82f, ImVec2(a.x + 40 * s, a.y + 58 * s),
                 since < 3.0f && !menu.photo_saved.empty() ? col::kFocusGreen : col::kWhite, hints,
                 1.5f * s, 1.4f * s);
}

void Draw() {
  namespace st = style;
  namespace col = style::color;
  const auto menu = current.load();
  if (menu && menu->photo_mode) {
    DrawPhotoMode(*menu);
    return;
  }
  if (menu && menu->motion_test)
    DrawMotionTest();
  if (menu)
    DrawSpinTest(*menu);
  if (menu && !menu->open && !menu->switching && menu->first_person_dot &&
      menu->first_person_active)
    DrawCentreDot();
  if (!menu || (!menu->open && !menu->switching))
    return;
  std::unique_lock lock(menu->mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  if (!menu->active)
    return;
  if (menu->switching) {
    DrawSwitchingPrompt(*menu);
    return;
  }
  const auto rows = Rows(*menu);
  menu->policy.set_rows(rows.size());
  const Page page = menu->policy.page();
  const bool list_page =
      page == Page::Settings || page == Page::Controls || page == Page::Shadow;

  const auto &io = ImGui::GetIO();
  ImFont *font = MenuFont();
  const float s = std::clamp(io.DisplaySize.y / 900.0f, 0.65f, 1.6f);
  // Darken the game behind, more towards the edges.
  {
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, st::Rgb(0, 6, 10, 150));
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y),
                                st::Rgb(0, 0, 0, 90), st::Rgb(0, 0, 0, 0), st::Rgb(0, 0, 0, 0),
                                st::Rgb(0, 0, 0, 90));
    bg->AddRectFilledMultiColor(ImVec2(io.DisplaySize.x * 0.5f, 0), io.DisplaySize,
                                st::Rgb(0, 0, 0, 0), st::Rgb(0, 0, 0, 90), st::Rgb(0, 0, 0, 90),
                                st::Rgb(0, 0, 0, 0));
  }

  const float width = std::min((list_page ? 900.0f : 760.0f) * s, io.DisplaySize.x - 32 * s);
  const float title_size = 38 * s, row_size = 26 * s, small_size = 20 * s;
  const float header_h = 74 * s, footer_h = 92 * s, gap = 14 * s;
  const float max_h = io.DisplaySize.y * 0.92f;

  // Row geometry: Start-menu buttons on the short pages, panels in the lists.
  const float button_h = 68 * s, button_gap = 12 * s;
  const float panel_h = 50 * s, panel_gap = 8 * s, section_h = 38 * s;
  float list_content_h = 0;
  if (list_page) {
    for (std::size_t row = 0; row < rows.size(); ++row)
      list_content_h += panel_h + panel_gap + (page == Page::Settings && SettingsSection(row) ? section_h : 0);
  } else {
    list_content_h = rows.size() * (button_h + button_gap);
  }
  const float controls_h = page == Page::Controls ? 300 * s : 0;
  const float list_h = std::min(list_content_h + 8 * s,
                                max_h - header_h - footer_h - controls_h - gap * 4);
  const float total_h = header_h + gap + list_h + gap + controls_h + footer_h;

  const ImVec2 origin((io.DisplaySize.x - width) * 0.5f, (io.DisplaySize.y - total_h) * 0.5f);
  ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, total_h), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::Begin("Colosseum pause", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList *draw = ImGui::GetWindowDrawList();

  // Header plate: page title, game name on the right.
  {
    const ImVec2 a = origin, b(origin.x + width, origin.y + header_h);
    st::ChromePlate(draw, a, b, 18 * s, 8 * s, col::kTealTop, col::kTealBottom);
    // Accent bar on the left, like the game's red title tabs.
    st::FillChamfer(draw, ImVec2(a.x + 14 * s, a.y + 16 * s), ImVec2(a.x + 24 * s, b.y - 16 * s),
                    3 * s, st::Rgb(240, 70, 60), st::Rgb(150, 20, 20));
    const char *title = page == Page::QuitConfirm      ? "LEAVE YOUR ADVENTURE?"
                        : page == Page::RestartConfirm ? "RESTART THE GAME?"
                        : page == Page::Settings       ? "SETTINGS"
                        : page == Page::Controls       ? "CONTROLS"
                        : page == Page::Shadow         ? "SHADOW POK\xC3\xA9MON"
                                                       : "PAUSE";
    const ImVec2 ts = st::TextSize(font, title_size, title);
    st::OutlinedText(draw, font, title_size, ImVec2(a.x + 40 * s, a.y + (header_h - ts.y) * 0.5f),
                     col::kWhite, title, 2.0f * s);
    const char *game = "POK\xC3\xA9MON COLOSSEUM";
    const ImVec2 gs = st::TextSize(font, small_size, game);
    st::OutlinedText(draw, font, small_size,
                     ImVec2(b.x - gs.x - 30 * s, a.y + (header_h - gs.y) * 0.5f), col::kFocusGreen,
                     game, 1.5f * s);
  }

  // Rows.
  const ImVec2 list_a(origin.x, origin.y + header_h + gap);
  ImGui::SetCursorScreenPos(list_a);
  ImGui::BeginChild("rows", ImVec2(width, list_h), false,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImDrawList *rows_draw = ImGui::GetWindowDrawList();
  const float scroll = ImGui::GetScrollY();
  const ImVec2 child_origin = ImGui::GetCursorScreenPos();
  float y = 4 * s;
  for (std::size_t row = 0; row < rows.size(); ++row) {
    ImGui::PushID(static_cast<int>(row));
    const bool focused = row == menu->policy.focus();
    const bool unavailable = page == Page::Settings &&
                             ((row >= 7 && row <= 9) || (row >= 17 && row <= 19)) &&
                             !menu->camera_available;
    float row_top, row_bottom;
    if (list_page) {
      if (page == Page::Settings) {
        if (const char *section = SettingsSection(row)) {
          const float sy = child_origin.y + y + section_h * 0.30f;
          st::OutlinedText(rows_draw, font, small_size, ImVec2(child_origin.x + 22 * s, sy),
                           col::kHeaderGreen, section, 1.2f * s);
          const float tx = child_origin.x + 34 * s + st::TextSize(font, small_size, section).x;
          const float ly = sy + small_size * 0.55f;
          rows_draw->AddRectFilledMultiColor(ImVec2(tx, ly), ImVec2(child_origin.x + width - 22 * s, ly + 2 * s),
                                             col::kChromeLight, st::Rgb(232, 236, 240, 0),
                                             st::Rgb(232, 236, 240, 0), col::kChromeLight);
          y += section_h;
        }
      }
      row_top = y;
      row_bottom = y + panel_h;
      const float inset = focused ? 0 : 10 * s;
      const ImVec2 a(child_origin.x + 8 * s + inset, child_origin.y + row_top);
      const ImVec2 b(child_origin.x + width - 8 * s, child_origin.y + row_bottom);
      const bool back = row + 1 == rows.size();
      st::Panel(rows_draw, a, b, 12 * s, (focused ? 4.0f : 3.0f) * s, focused,
                back ? col::kTealTop : (focused ? st::Rgb(30, 96, 66) : col::kGreenTop),
                back ? col::kTealBottom : (focused ? st::Rgb(14, 56, 40) : col::kGreenBottom));
      auto [label, value] = SplitRow(rows[row]);
      const ImU32 dim = unavailable ? st::Rgb(140, 150, 146) : col::kWhite;
      const ImVec2 ls = st::TextSize(font, row_size, label);
      const float ty = a.y + (panel_h - ls.y) * 0.5f;
      if (focused) {
        // Pointer like the game's cursor.
        const float cx = a.x + 20 * s, cy = a.y + panel_h * 0.5f;
        rows_draw->AddTriangleFilled(ImVec2(cx - 6 * s, cy - 8 * s), ImVec2(cx - 6 * s, cy + 8 * s),
                                     ImVec2(cx + 6 * s, cy), col::kFocusGreen);
      }
      st::OutlinedText(rows_draw, font, row_size, ImVec2(a.x + 36 * s, ty), dim, label, 1.5f * s);
      if (!value.empty()) {
        ImU32 value_color = col::kYellow;
        std::string shown = value;
        if (value == "ON") {
          value_color = col::kFocusGreen;
        } else if (value == "OFF") {
          value_color = col::kRed;
        } else if (page == Page::Shadow) {
          value_color = value == "PURIFIED"  ? st::Rgb(150, 220, 255)
                        : value == "SNAGGED" ? col::kYellow
                        : value == "SEEN"    ? col::kRed
                                             : st::Rgb(120, 140, 132);
        }
        // Shadow Pokemon you own: the heart gauge left of the status.
        if (page == Page::Shadow && row < menu->shadow.entries.size() &&
            menu->shadow.entries[row].heart >= 0) {
          const float fraction = menu->shadow.entries[row].heart;
          const float bw = 150 * s, bh = 12 * s;
          const float bx = b.x - st::TextSize(font, row_size, value).x - 52 * s - bw;
          const float by = a.y + (panel_h - bh) * 0.5f;
          rows_draw->AddRectFilled(ImVec2(bx - 2 * s, by - 2 * s), ImVec2(bx + bw + 2 * s, by + bh + 2 * s),
                                   col::kChromeEdge, 3 * s);
          rows_draw->AddRectFilledMultiColor(ImVec2(bx, by), ImVec2(bx + bw * fraction, by + bh),
                                             st::Rgb(170, 70, 230), st::Rgb(110, 30, 170),
                                             st::Rgb(110, 30, 170), st::Rgb(170, 70, 230));
          st::OutlinedText(rows_draw, font, small_size * 0.85f,
                           ImVec2(bx - 70 * s, by - 3 * s),
                           menu->shadow.entries[row].in_party ? col::kWhite : st::Rgb(170, 186, 178),
                           menu->shadow.entries[row].in_party ? "PARTY" : "PC", 1.2f * s);
        }
        const ImVec2 vs = st::TextSize(font, row_size, shown);
        const float vx = b.x - vs.x - 28 * s;
        if (focused && page != Page::Shadow) {
          // Arrows hint that Left / Right change it.
          const float cy = a.y + panel_h * 0.5f;
          rows_draw->AddTriangleFilled(ImVec2(vx - 22 * s, cy), ImVec2(vx - 12 * s, cy - 7 * s),
                                       ImVec2(vx - 12 * s, cy + 7 * s), col::kChromeLight);
          rows_draw->AddTriangleFilled(ImVec2(b.x - 10 * s, cy), ImVec2(b.x - 20 * s, cy - 7 * s),
                                       ImVec2(b.x - 20 * s, cy + 7 * s), col::kChromeLight);
        }
        st::OutlinedText(rows_draw, font, row_size, ImVec2(vx, ty), unavailable ? dim : value_color,
                         shown, 1.5f * s);
      }
      y = row_bottom + panel_gap;
    } else {
      // Start menu buttons: the focused one pops out to the left in green.
      row_top = y;
      row_bottom = y + button_h;
      const float rail_x = child_origin.x + width - 70 * s;
      const float bw = width * 0.68f;
      const float shift = focused ? 34 * s : 0;
      const ImVec2 a(rail_x - bw - shift, child_origin.y + row_top);
      const ImVec2 b(rail_x - shift, child_origin.y + row_bottom);
      st::ChromePlate(rows_draw, a, b, 18 * s, 8 * s, col::kTealTop, col::kTealBottom);
      const std::string label = Upper(rows[row]);
      const ImVec2 ls = st::TextSize(font, row_size * 1.3f, label);
      const ImVec2 tp(a.x + (bw - ls.x) * 0.5f, a.y + (button_h - ls.y) * 0.5f);
      if (focused)
        st::OutlinedText(rows_draw, font, row_size * 1.3f, tp, col::kFocusGreen, label, 2.0f * s,
                         st::Rgb(4, 30, 10, 240));
      else
        st::EngravedText(rows_draw, font, row_size * 1.3f, tp, col::kDimTeal, label, 1.5f * s);
      y = row_bottom + button_gap;
    }
    // Mouse: the whole row is a button.
    ImGui::SetCursorPos(ImVec2(0, row_top));
    if (ImGui::InvisibleButton("row", ImVec2(width, row_bottom - row_top)) && !unavailable)
      QueueActivate(menu, row);
    if (focused && menu->scrolled_focus != row) {
      ImGui::SetScrollY(std::clamp((row_top + row_bottom) * 0.5f - list_h * 0.5f, 0.0f,
                                   std::max(0.0f, list_content_h - list_h)));
      menu->scrolled_focus = row;
    }
    ImGui::PopID();
  }
  ImGui::SetCursorPos(ImVec2(0, y));
  ImGui::Dummy(ImVec2(1, 4 * s));
  // The chrome rail behind the Start menu buttons.
  if (!list_page && !rows.empty()) {
    const float rx = child_origin.x + width - 70 * s;
    const ImVec2 a(rx, child_origin.y + 4 * s - 10 * s), b(rx + 40 * s, child_origin.y + y + 10 * s - button_gap);
    // Chrome rail with a notch facing each button and a slot that lights up.
    st::FillChamfer(rows_draw, ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), 9 * s,
                    col::kChromeEdge, col::kChromeEdge);
    st::FillChamfer(rows_draw, a, b, 8 * s, col::kChromeLight, col::kChromeDark);
    rows_draw->AddRectFilledMultiColor(ImVec2(a.x + 4 * s, a.y + 6 * s), ImVec2(a.x + 9 * s, b.y - 6 * s),
                                       st::Rgb(255, 255, 255, 120), st::Rgb(255, 255, 255, 0),
                                       st::Rgb(255, 255, 255, 0), st::Rgb(255, 255, 255, 120));
    for (std::size_t row = 0; row < rows.size(); ++row) {
      const float cy = child_origin.y + 4 * s + row * (button_h + button_gap) + button_h * 0.5f;
      const bool lit = row == menu->policy.focus();
      rows_draw->AddTriangleFilled(ImVec2(a.x + 2 * s, cy), ImVec2(a.x + 12 * s, cy - 9 * s),
                                   ImVec2(a.x + 12 * s, cy + 9 * s), col::kChromeEdge);
      rows_draw->AddRectFilled(ImVec2(rx + 16 * s, cy - 6 * s), ImVec2(rx + 36 * s, cy + 6 * s),
                               col::kChromeEdge, 2 * s);
      rows_draw->AddRectFilled(ImVec2(rx + 18 * s, cy - 4 * s), ImVec2(rx + 34 * s, cy + 4 * s),
                               lit ? col::kFocusGreen : st::Rgb(16, 26, 30), 1 * s);
    }
  }
  (void)scroll;
  ImGui::EndChild();

  // Controls page: the button guide on a panel.
  float footer_y = list_a.y + list_h + gap;
  if (page == Page::Controls) {
    const ImVec2 a(origin.x + 8 * s, footer_y), b(origin.x + width - 8 * s, footer_y + controls_h - gap);
    st::Panel(draw, a, b, 12 * s, 3 * s, false);
    static const char *const kGuide[][2] = {
        {"Start + Z / Esc", "Pause menu"},          {"D-pad / Arrows", "Move"},
        {"A / Enter", "Select"},                    {"B / Backspace", "Back"},
        {"C-stick", "Orbit the camera"},            {"Z + Y", "Free camera"},
        {"Z + X", "First person"},                  {"Left stick click", "Run"},
        {"Left stick", "Free cam: fly"},          {"C-stick", "Free cam: look"},
        {"R / L", "Free cam: rise / sink"},          {"Z + D-pad Down", "Reset camera"},
    };
    // One entry per line: the button in yellow, what it does in white.
    const float guide_size = row_size * 0.82f;
    const float column = (b.x - a.x) * 0.5f;
    constexpr int kEntries = static_cast<int>(std::size(kGuide));
    for (int i = 0; i < kEntries; ++i) {
      const float cx = a.x + 26 * s + (i % 2) * column;
      const float cy = a.y + 16 * s + (i / 2) * ((b.y - a.y - 32 * s) / (kEntries / 2));
      st::OutlinedText(draw, font, guide_size, ImVec2(cx, cy), col::kYellow, kGuide[i][0], 1.3f * s);
      st::OutlinedText(draw, font, guide_size, ImVec2(cx + column * 0.48f, cy), col::kWhite,
                       kGuide[i][1], 1.3f * s);
    }
    footer_y += controls_h;
  }

  // Message box: status, or what the buttons do.
  {
    const ImVec2 a(origin.x, footer_y), b(origin.x + width, footer_y + footer_h - 6 * s);
    st::ChromePlate(draw, a, b, 16 * s, 7 * s, st::Rgb(20, 70, 64), st::Rgb(10, 40, 38));
    std::string message = menu->status;
    if (page == Page::Shadow && message.empty()) {
      const auto &sh = menu->shadow;
      message = sh.valid ? "Snagged " + std::to_string(sh.snagged) + " / " + std::to_string(sh.total) +
                               "    Purified " + std::to_string(sh.purified) + "    Seen " +
                               std::to_string(sh.seen)
                         : "Load your adventure to see your Shadow Pok\xC3\xA9mon.";
    }
    if (message.empty())
      message = list_page ? "Left / Right: change    A: select    B: back"
                          : "A: select    B: back    Start: resume";
    const float spacing = 2.0f * s;
    float msize = row_size * 0.9f;
    const float avail = width - 60 * s;
    while (msize > small_size * 0.8f && st::SpacedWidth(font, msize, message, spacing) > avail)
      msize -= 1.0f;
    st::SpacedText(draw, font, msize, ImVec2(a.x + 30 * s, a.y + (b.y - a.y - msize) * 0.5f),
                   col::kWhite, message, spacing, 1.5f * s);
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}
} // namespace moderngekko::pause_menu

// Save states hold guest memory only; the camera mod's angle (orbit yaw,
// pitch, zoom, free camera) lives in the mod, so a state reloaded with
// another view. Keep it next to the state as <state>.camera.
void Host_SaveCameraForState(const std::string &state_path) {
  const auto menu = moderngekko::pause_menu::current.load();
  if (!menu || state_path.empty())
    return;
  const std::string encoded = moderngekko::pause_menu::ReadCameraState(menu);
  if (encoded.empty())
    return;
  std::ofstream out(state_path + ".camera", std::ios::trunc);
  out << encoded << '\n';
}
void Host_RestoreCameraForState(const std::string &state_path) {
  const auto menu = moderngekko::pause_menu::current.load();
  if (!menu || state_path.empty())
    return;
  std::ifstream in(state_path + ".camera");
  std::string encoded;
  if (in && std::getline(in, encoded) && !encoded.empty())
    moderngekko::pause_menu::RestoreCameraState(menu, encoded);
}

void Host_PauseMenuKey(int key, bool down) {
  const auto menu = moderngekko::pause_menu::current.load();
  if (menu && key >= 0 && key < 256) {
    if (down && !menu->keys[key].exchange(true))
      menu->pressed_keys[key] = true;
    if (!down)
      menu->keys[key] = false;
  }
}
bool Host_IsPauseMenuOpen() {
  const auto menu = moderngekko::pause_menu::current.load();
  return menu && menu->open;
}
void Host_PauseMenuFocusLost() {
  if (const auto menu = moderngekko::pause_menu::current.load()) {
    for (auto &key : menu->keys)
      key = false;
    for (auto &key : menu->pressed_keys)
      key = false;
    menu->toggle = false;
  }
}
bool Host_IsPauseMenuInputBlocked() {
  const auto menu = moderngekko::pause_menu::current.load();
  return menu && (menu->blocked || menu->toggle);
}
void Host_RequestPauseMenuToggle() {
  if (const auto menu = moderngekko::pause_menu::current.load())
    menu->toggle = true;
}

void Host_PauseMenuTick() {
  using namespace moderngekko::pause_menu;
  const auto menu = current.load();
  if (!menu || !menu->active || menu->switching)
    return;
  const auto poll_time = std::chrono::steady_clock::now();
  PublishLookInput(*menu, poll_time);
  if (poll_time - menu->last_poll < std::chrono::milliseconds(16))
    return;
  menu->last_poll = poll_time;
  UpdateDepthOfField(menu);
  // Auto first-person turn speed follows the frame rate, the field of view and
  // the window's aspect ratio (fullscreen / ultrawide changes).
  if (menu->first_person_sensitivity == 0) {
    const int automatic = AutoFirstPersonSensitivity(*menu);
    if (automatic != menu->applied_auto_sensitivity) {
      menu->applied_auto_sensitivity = automatic;
      std::fprintf(stderr, "[camera] first-person turn speed auto %d%%\n", automatic);
      Camera(menu);
    }
  }
  GCPadStatus pad{};
  if (Pad::IsInitialized()) {
    g_controller_interface.UpdateInput();
    polling_menu = true;
    pad = Pad::GetStatus(0);
    polling_menu = false;
  }
  std::lock_guard lock(menu->mutex);
  if (menu->photo_mode) {
    HandlePhotoMode(menu, pad);
    return;
  }
  Input input;
  Input pressed;
  pressed.escape = menu->pressed_keys[27].exchange(false);
  pressed.up = menu->pressed_keys[38].exchange(false);
  pressed.down = menu->pressed_keys[40].exchange(false);
  pressed.accept = menu->pressed_keys[13].exchange(false);
  pressed.back = menu->pressed_keys[8].exchange(false);
  menu->policy.reset_edges(pressed);
  input.escape = menu->keys[27] || pressed.escape;
  input.up = menu->keys[38] || pressed.up || (pad.button & PAD_BUTTON_UP) ||
             pad.stickY > 192;
  input.down = menu->keys[40] || pressed.down ||
               (pad.button & PAD_BUTTON_DOWN) || pad.stickY < 64;
  input.accept =
      menu->keys[13] || pressed.accept || (pad.button & PAD_BUTTON_A);
  input.back = menu->keys[8] || pressed.back || (pad.button & PAD_BUTTON_B);
  input.start = pad.button & PAD_BUTTON_START;
  input.select = pad.button & PAD_TRIGGER_Z;
  input.any_game_button =
      menu->keys[37] || menu->keys[39] || pad.button || pad.triggerLeft ||
      pad.triggerRight || std::abs(static_cast<int>(pad.stickX) - 128) > 32 ||
      std::abs(static_cast<int>(pad.stickY) - 128) > 32 ||
      std::abs(static_cast<int>(pad.substickX) - 128) > 32 ||
      std::abs(static_cast<int>(pad.substickY) - 128) > 32;
  const auto state = Core::GetState(Core::System::GetInstance());
  input.game_running =
      state == Core::State::Running || state == Core::State::Paused;
  const bool was_open = menu->policy.is_open();
  // Back / Select / Minus: open straight to Settings, or close the menu.
  {
    const bool select = input.game_running && SelectButtonDown();
    // A controller whose Back button is also mapped to Z reports Start + Z
    // as both; let the chord (policy.update below) do the one toggle, or the
    // menu opens and closes in the same tick (game paused, menu not shown).
    const bool chord = input.start && input.select;
    if (select && !menu->select_was_down && !chord) {
      if (menu->policy.is_open()) {
        menu->policy.close();
      } else {
        menu->policy.open();
        menu->policy.activate(1);  // Home row 1 = Settings
      }
    }
    menu->select_was_down = select;
  }
  // Left stick click toggles running while playing.
  {
    const bool click = input.game_running && !menu->policy.is_open() && LeftStickClickDown();
    if (click && !menu->run_click_was_down) {
      menu->running = !menu->running;
      menu->status = menu->running ? "Running" : "Walking";
      std::fprintf(stderr, "[player] run %s\n", menu->running ? "on" : "off");
      SetRun(menu, menu->running);
    }
    menu->run_click_was_down = click;
  }
  const bool requested_toggle = menu->toggle.exchange(false);
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const auto page = menu->policy.page();
  if (page == Page::Shadow && poll_time - menu->shadow_read > std::chrono::seconds(1)) {
    menu->shadow = moderngekko::shadow::Read();
    menu->shadow_read = poll_time;
  }
  const std::size_t rows = page == Page::Settings   ? kSettingsRows
                           : page == Page::Controls ? 2
                           : page == Page::Shadow   ? menu->shadow.entries.size() + 1
                                                    : 0;
  ActionHost(menu, menu->policy.update(input, now, rows));
  // A host command and an input edge in the same tick describe one toggle.
  if (requested_toggle && input.game_running &&
      menu->policy.is_open() == was_open) {
    if (was_open)
      menu->policy.close();
    else
      menu->policy.open();
  }
  const bool left_pressed = menu->pressed_keys[37].exchange(false);
  const bool right_pressed = menu->pressed_keys[39].exchange(false);
  if (left_pressed || right_pressed)
    menu->adjust_direction = 0;
  const bool left = menu->keys[37] || left_pressed ||
                    (pad.button & PAD_BUTTON_LEFT) || pad.stickX < 64;
  const bool right = menu->keys[39] || right_pressed ||
                     (pad.button & PAD_BUTTON_RIGHT) || pad.stickX > 192;
  const int direction = left == right ? 0 : left ? -1 : 1;
  if (direction &&
      (direction != menu->adjust_direction || now >= menu->next_adjust) &&
      (menu->policy.page() == Page::Settings ||
       menu->policy.page() == Page::Controls) &&
      menu->policy.focus() + 1 < menu->policy.row_count()) {
    Adjust(menu, direction);
    menu->next_adjust =
        now + (direction != menu->adjust_direction ? 0.35 : 0.10);
  }
  menu->adjust_direction = direction;
  SyncPause(menu, was_open);
  const auto time = std::chrono::steady_clock::now();
  if (menu->open &&
      Core::GetState(Core::System::GetInstance()) == Core::State::Paused &&
      time - menu->last_repaint >= std::chrono::milliseconds(16) &&
      !menu->repaint_pending.exchange(true)) {
    menu->last_repaint = time;
    Core::RunOnCPUThread(Core::System::GetInstance(),
                         [weak = std::weak_ptr(menu)] {
                           const auto state = weak.lock();
                           if (!state || !state->active)
                             return;
                           AsyncRequests::GetInstance()->PushEvent([weak] {
                             if (const auto state = weak.lock()) {
                               if (state->active && state->open && g_presenter)
                                 g_presenter->Present();
                               state->repaint_pending = false;
                             }
                           });
                         });
  }
}
