// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/Host.h"
#include "Core/SavestateLayout.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/runtime.hpp"
#include "amd_driver_settings.h"
#include "pause_menu_host.hpp"
#include "pause_menu_policy.hpp"
#include <cstdio>
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
  std::atomic<bool> repaint_pending{false};
  ModManager *mods = nullptr;
  std::string config_path;
  // config.ini [Video] backend (the Graphics API used from the next launch).
  std::string graphics_backend = "Vulkan";
  // Graphics API switch in progress: the menu shows a reloading prompt, the
  // game stays paused and input is ignored until the new session takes over.
  std::atomic<bool> switching{false};
  // First-person depth of field: 0 off, 1 very low .. 5 very high (config.ini
  // [Camera] DepthOfField).
  int depth_of_field = 0;
  // First-person horizontal FOV at 16:9 in degrees (0 = game default) and the
  // centre dot (config.ini [Camera] FirstPersonFov / FirstPersonDot).
  int first_person_fov = 0;
  bool first_person_dot = true;
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
};
std::atomic<std::shared_ptr<Menu>> current;
thread_local bool polling_menu = false;

void Filter(int port, GCPadStatus &pad) {
  if (port != 0 || polling_menu)
    return;
  const auto menu = current.load();
  if (!menu || !menu->active)
    return;
  const bool chord = (pad.button & (PAD_BUTTON_START | PAD_TRIGGER_Z)) ==
                     (PAD_BUTTON_START | PAD_TRIGGER_Z);
  if (menu->blocked || menu->toggle || menu->switching || chord) {
    const bool connected = pad.isConnected;
    pad = GCPadStatus{};
    pad.isConnected = connected;
  }
}

void Camera(const std::shared_ptr<Menu> &menu, bool recenter = false) {
  const int sensitivity = menu->sensitivity;
  const bool invert = menu->invert_y;
  Core::RunOnCPUThread(
      Core::System::GetInstance(),
      [weak = std::weak_ptr(menu), sensitivity, invert, recenter] {
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

void SetFrameRate(const std::shared_ptr<Menu> &menu, int rate) {
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

// Radeon settings rows (20..25) in amd::Feature order.
constexpr int kFirstAmdRow = 20;

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
  if (((menu->policy.focus() >= 6 && menu->policy.focus() <= 8) || (menu->policy.focus() >= 16 && menu->policy.focus() <= 18)) &&
      !menu->camera_available) {
    menu->status = "Camera controls require the camera mod.";
    return;
  }
  switch (menu->policy.focus()) {
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
  case 11: {
    const int scale =
        std::clamp(Config::Get(Config::GFX_EFB_SCALE) + direction, 1, 6);
    Common::IniFile ini;
    ini.Load(menu->config_path);
    ini.GetOrCreateSection("Performance")->Set("ResolutionScale", scale);
    ini.GetOrCreateSection("Video")->Set("resolution",
                                         std::to_string(640 * scale) + "x" +
                                             std::to_string(528 * scale));
    ini.Save(menu->config_path);
    Core::RunOnCPUThread(Core::System::GetInstance(),
                         [scale] { Host_SetPauseMenuResolution(scale); });
    break;
  }
  case 12:
    Config::SetBase(Config::GFX_VSYNC, !Config::Get(Config::GFX_VSYNC));
    Config::Save();
    break;
  case 13: {
    const bool show = !Config::Get(Config::GFX_SHOW_FPS);
    Config::SetBase(Config::GFX_SHOW_FPS, show);
    Config::SetBase(Config::GFX_SHOW_FTIMES, show);
    Config::SetBase(Config::GFX_SHOW_SPEED, show);
    Config::SetBase(Config::GFX_SHOW_INTERNAL_RESOLUTION, show);
    Config::Save();
    break;
  }
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
  case kFirstAmdRow + 0:
  case kFirstAmdRow + 1:
  case kFirstAmdRow + 2:
  case kFirstAmdRow + 3:
  case kFirstAmdRow + 4:
  case kFirstAmdRow + 5:
    CycleAmdSetting(menu, static_cast<int>(menu->policy.focus()) - kFirstAmdRow, direction);
    break;
  default:
    break;
  }
}

void ActionHost(const std::shared_ptr<Menu> &menu, Action action,
                [[maybe_unused]] int direction = 1) {
  switch (action) {
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
      menu->status = "Checkpoint save requested";
    } else
      menu->status = "Unable to create checkpoint folder";
    break;
  }
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
    return {"Resume adventure", "Settings", "Controls", "Save checkpoint",
            "Quit game"};
  case Page::Settings:
    return {
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
        AmdLabel(menu, amd::Feature::AntiLag),
        AmdLabel(menu, amd::Feature::Chill),
        AmdLabel(menu, amd::Feature::Boost),
        AmdLabel(menu, amd::Feature::ImageSharpening),
        AmdLabel(menu, amd::Feature::EnhancedSync),
        AmdLabel(menu, amd::Feature::FluidMotionFrames),
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
  section->Get("FirstPersonFov", &menu->first_person_fov, 0);
  section->Get("FirstPersonDot", &menu->first_person_dot, true);
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
  ApplyFirstPersonFov(menu);
  // MODERNGEKKO_CAMERA_STATE: camera angle handed over by the Graphics API
  // switch; restored before the first field update.
  if (const char *camera_state = std::getenv("MODERNGEKKO_CAMERA_STATE");
      camera_state && *camera_state) {
    RestoreCameraState(menu, camera_state);
#ifdef _WIN32
    SetEnvironmentVariableA("MODERNGEKKO_CAMERA_STATE", nullptr);
#endif
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

void Draw() {
  const auto menu = current.load();
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
  const auto &io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                IM_COL32(3, 10, 27, 170));
  const float scale = std::clamp(io.DisplaySize.y / 900.0f, 0.65f, 1.5f);
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(
      ImVec2(std::min(820.0f * scale, io.DisplaySize.x - 24), 0),
      ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImVec4(0.025f, 0.065f, 0.17f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.80f, 0.18f, 1));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.96f, 1, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(24 * scale, 20 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2);
  ImGui::Begin("Colosseum pause", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::SetWindowFontScale(scale * 1.5f);
  ImGui::TextColored(ImVec4(1, 0.83f, 0.25f, 1), "POKEMON COLOSSEUM");
  ImGui::TextUnformatted(
      menu->policy.page() == Page::QuitConfirm ? "Leave your adventure?"
      : menu->policy.page() == Page::Settings  ? "Settings"
      : menu->policy.page() == Page::Controls  ? "Controls"
                                               : "Adventure paused");
  ImGui::Separator();
  // The row list scrolls inside a frame that always fits on screen (Settings
  // has more rows than fit at 1440p with this font scale); the focused row is
  // kept in view.
  const float row_height = 28 * scale + ImGui::GetStyle().ItemSpacing.y;
  const float max_list_height = io.DisplaySize.y * 0.86f - ImGui::GetCursorPosY() -
                                6.0f * ImGui::GetTextLineHeightWithSpacing();
  const float list_height =
      std::min(row_height * static_cast<float>(rows.size()), std::max(max_list_height, row_height * 4));
  ImGui::BeginChild("rows", ImVec2(0, list_height), false);
  for (std::size_t row = 0; row < rows.size(); ++row) {
    ImGui::PushID(static_cast<int>(row));
    const bool focused = row == menu->policy.focus();
    const bool unavailable = menu->policy.page() == Page::Settings &&
                             ((row >= 6 && row <= 8) || (row >= 16 && row <= 18)) && !menu->camera_available;
    ImGui::BeginDisabled(unavailable);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.32f, 0.52f, 1));
    const std::string label = (focused ? ">  " : "   ") + rows[row];
    if (ImGui::Selectable(label.c_str(), focused, 0, ImVec2(0, 28 * scale))) {
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
    if (focused && menu->scrolled_focus != row) {
      ImGui::SetScrollHereY(0.5f);
      menu->scrolled_focus = row;
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::EndDisabled();
  }
  ImGui::EndChild();
  if (menu->policy.page() == Page::Controls) {
    ImGui::Separator();
    ImGui::TextWrapped(
        "Start + Z / Esc: pause menu\nD-pad / arrows: navigate    A / Enter: "
        "select\nB / Backspace: back    Left / Right: change device\nCamera: "
        "C-stick orbit    Z + Y: free camera\nZ + D-pad Down: recenter    "
        "D-pad Up / Down: zoom\nExisting controller button bindings are "
        "preserved.");
  }
  ImGui::Separator();
  if (!menu->status.empty())
    ImGui::TextWrapped("%s", menu->status.c_str());
  if (menu->policy.page() == Page::Settings ||
      menu->policy.page() == Page::Controls)
    ImGui::TextWrapped("Up / Down: move    Left / Right: adjust\nEnter / A: "
                       "select    Esc: resume    B: back");
  else
    ImGui::TextWrapped("Arrows / D-pad: move    Enter / A: select\nEsc: resume "
                       "   Backspace / B: back");
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(3);
}
} // namespace moderngekko::pause_menu

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
  if (poll_time - menu->last_poll < std::chrono::milliseconds(16))
    return;
  menu->last_poll = poll_time;
  UpdateDepthOfField(menu);
  GCPadStatus pad{};
  if (Pad::IsInitialized()) {
    g_controller_interface.UpdateInput();
    polling_menu = true;
    pad = Pad::GetStatus(0);
    polling_menu = false;
  }
  std::lock_guard lock(menu->mutex);
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
    if (select && !menu->select_was_down) {
      if (menu->policy.is_open()) {
        menu->policy.close();
      } else {
        menu->policy.open();
        menu->policy.activate(1);  // Home row 1 = Settings
      }
    }
    menu->select_was_down = select;
  }
  const bool requested_toggle = menu->toggle.exchange(false);
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const auto page = menu->policy.page();
  const std::size_t rows = page == Page::Settings   ? 27
                           : page == Page::Controls ? 2
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
