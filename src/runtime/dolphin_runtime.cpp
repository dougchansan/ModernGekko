#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/GBACore.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/HW/GCPad.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;

// Benchmark automation state, driven by a dedicated thread in Runtime::Run.
//
// It deliberately does not piggyback on the title-update thread: that one only
// exists when a window is present and show_fps_in_title is set, so a headless
// run would tick exactly once and report a permanent speed of 0. Headless is
// the mode a benchmark most wants.
//
// State::LoadAs and SaveAs both hop to the CPU thread via Core::RunOnCPUThread,
// so calling them from here is safe.
std::filesystem::path s_status_file;
std::filesystem::path s_load_state_path;
std::filesystem::path s_save_state_path;
double s_load_state_after_seconds = 5.0;
double s_save_state_after_seconds = 0.0;
double s_run_seconds = 0.0;
double s_screenshot_after_seconds = 0.0;
std::filesystem::path s_screenshot_trigger;
unsigned int s_hold_buttons = 0;
unsigned int s_spam_buttons = 0;
double s_spam_period_seconds = 0.25;
double s_spam_stop_seconds = 0.0;
bool s_spam_pressed = false;
double s_spam_next_toggle = 0.0;
u64 s_spam_next_tick = 0;
bool s_hold_applied = false;
bool s_load_state_done = false;
bool s_save_state_done = false;
bool s_screenshot_done = false;
bool s_automation_started = false;
u64 s_status_updates = 0;
std::chrono::steady_clock::time_point s_automation_start;

void RunAutomationTick() {
  if (s_status_file.empty() && s_load_state_path.empty() &&
      s_save_state_path.empty() && s_run_seconds <= 0.0 && s_hold_buttons == 0 &&
      s_spam_buttons == 0)
    return;

  const auto now = std::chrono::steady_clock::now();
  if (!s_automation_started) {
    s_automation_started = true;
    s_automation_start = now;
  }
  const double elapsed =
      std::chrono::duration<double>(now - s_automation_start).count();

  auto &system = Core::System::GetInstance();

  // Re-apply every tick rather than once: Pad::LoadConfig during boot
  // reconstructs the controllers, and a hold applied before that is lost.
  if (s_hold_buttons != 0 && (!s_hold_applied || !s_load_state_done)) {
    Pad::SetHeldButtons(0, static_cast<u16>(s_hold_buttons));
    s_hold_applied = true;
  }

  // Menus advance on the press edge, so a held button is not a press: it goes
  // down once and then reads as permanently down, which advances at most one
  // screen. Release between presses so each cycle produces a fresh edge, and
  // a fresh boot can be walked through to a race unattended.
  if (s_spam_buttons != 0 && s_spam_stop_seconds > 0.0 &&
      elapsed >= s_spam_stop_seconds) {
    s_spam_buttons = 0;
    Pad::SetHeldButtons(0, static_cast<u16>(s_hold_buttons));
    std::fprintf(stderr, "[spam] stopped at %.0fs\n", elapsed);
  }

  // Drive the spam from EMULATED time, not wall clock. This tick runs on a
  // real-time thread, so a wall-clock cadence lands at wildly different points
  // in the game depending on how fast the build runs -- 3.7x uncapped versus
  // ~1x with the write journal. That made the same savestate walk different
  // menu paths run to run, which read as an intermittent "race" until the cause
  // was traced back to here. Emulated ticks make a given state reproducible.
  // Hold off until the state is loaded, and re-sync afterwards: loading jumps
  // the tick counter to the state's value, so a schedule started at boot is
  // stale the moment the state lands -- which reintroduced exactly the
  // wall-clock dependence this change was meant to remove.
  if (s_spam_buttons != 0 && !s_load_state_path.empty() && !s_load_state_done) {
    // no input before the state is in place
  } else if (s_spam_buttons != 0) {
    const u64 ticks = system.GetCoreTiming().GetTicks();
    const u64 period =
        static_cast<u64>(s_spam_period_seconds *
                         static_cast<double>(system.GetSystemTimers().GetTicksPerSecond()));
    if (period != 0 && ticks >= s_spam_next_tick) {
      s_spam_pressed = !s_spam_pressed;
      s_spam_next_tick = ticks + period;
      // Release to nothing, not to the hold mask. --hold-accelerate holds A,
      // and an A that never lifts produces no press edge, so combining the two
      // would silently stop the spam from advancing anything.
      Pad::SetHeldButtons(0, s_spam_pressed ? static_cast<u16>(s_spam_buttons) : 0);
    }
  }

  if (!s_load_state_path.empty() && !s_load_state_done &&
      elapsed >= s_load_state_after_seconds) {
    s_load_state_done = true;
    State::LoadAs(system, s_load_state_path.string());
    s_spam_next_tick = system.GetCoreTiming().GetTicks();   // re-sync after the jump
  }

  if (!s_save_state_path.empty() && !s_save_state_done &&
      elapsed >= s_save_state_after_seconds) {
    s_save_state_done = true;
    State::SaveAs(system, s_save_state_path.string());
  }

  const bool screenshot_time_reached =
      s_screenshot_after_seconds > 0.0 && elapsed >= s_screenshot_after_seconds;
  const bool screenshot_triggered =
      !s_screenshot_trigger.empty() && std::filesystem::exists(s_screenshot_trigger);
  if (!s_screenshot_done && (screenshot_time_reached || screenshot_triggered)) {
    s_screenshot_done = true;
    // This tick is deliberately off the CPU thread. Core::SaveScreenShot()
    // acquires a CPUThreadGuard and can deadlock here; FrameDumper's request
    // path is explicitly protected by its own mutex and consumed by video.
    const char* path = std::getenv("KART_SCREENSHOT_PATH");
    g_frame_dumper->SaveScreenshot(path ? path : "/tmp/kart-nine-course.png");
    std::fprintf(stderr, "[screenshot] requested at %.0fs\n", elapsed);
  }

  if (!s_status_file.empty()) {
    const auto &metrics = system.GetPerfMetrics();
    // Rewrite in place rather than append: a poller only ever wants the
    // latest sample. It can read a partial write, so the reader retries --
    // see the benchmark harness.
    const std::filesystem::path temporary =
        s_status_file.string() + ".tmp";
    {
      std::ofstream out(temporary, std::ios::trunc);
      if (out) {
        out << "updates=" << ++s_status_updates << '\n'
            << "elapsed=" << elapsed << '\n'
            << "speed=" << metrics.GetSpeed() << '\n'
            << "max_speed=" << metrics.GetMaxSpeed() << '\n'
            << "vps=" << metrics.GetVPS() << '\n'
            << "fps=" << metrics.GetFPS() << '\n'
            << "state_loaded=" << (s_load_state_done ? 1 : 0) << '\n';
      }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, s_status_file, rename_error);
  }

  // The Windows automation `stop` command was accepted but never exited the
  // runner, and killing it instead skipped atexit hooks. Exiting on our own
  // deadline avoids needing a kill at all.
  if (s_run_seconds > 0.0 && elapsed >= s_run_seconds && s_platform)
    s_platform->Stop();
}

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  const auto now = std::chrono::steady_clock::now();
  std::string formatted_title = fmt::format("{} | {:.1f} FPS", title, fps);
  const NetPlay::InputWaitTelemetry telemetry =
      NetPlay::NetPlayClient::GetInputWaitTelemetry();
  if (!telemetry.active) {
    s_previous_net_wait_ns = 0;
    s_net_wait_ms_per_second = 0.0;
    s_previous_net_wait_sample = {};
    return formatted_title;
  }
  if (s_previous_net_wait_sample.time_since_epoch().count() == 0) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  } else if (telemetry.total_wait_ns < s_previous_net_wait_ns) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
    s_net_wait_ms_per_second = 0.0;
  } else if (now - s_previous_net_wait_sample >=
             std::chrono::milliseconds(500)) {
    const double seconds =
        std::chrono::duration<double>(now - s_previous_net_wait_sample).count();
    s_net_wait_ms_per_second =
        static_cast<double>(telemetry.total_wait_ns - s_previous_net_wait_ns) /
        1000000.0 / seconds;
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  }
  return fmt::format("{} | Net wait {:.1f} ms/s | Buffer {}", formatted_title,
                     s_net_wait_ms_per_second, telemetry.buffer_size);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(
        title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  std::unique_ptr<ModManager> mods;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
};

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      "ModernGekko - " + impl->metadata.game_name + " [" +
      impl->metadata.disc_id + "]");
  impl->mods = std::make_unique<ModManager>();
  const ModLoadReport mod_report = impl->mods->LoadDirectories(
      impl->config.mod_directories, impl->metadata.disc_id);
  for (const ModLoadIssue &issue : mod_report.issues)
    std::fprintf(stderr, "mod rejected: %s: %s\n", issue.source.c_str(),
                 issue.message.c_str());
  for (const LoadedModInfo &mod : mod_report.loaded)
    std::fprintf(stderr, "mod loaded: %s %s\n", mod.id.c_str(),
                 mod.version.c_str());

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(impl->config.user_directory.string());
    // UICommon::Init() does not create the user directory tree; DolphinQt and
    // the Android frontend both call CreateDirectories() themselves and this
    // runner never did. StateSaves/ is only created here, so State::Save had
    // nowhere to write and every savestate from the States menu failed
    // silently -- no file, no error. Screenshots, logs and shader caches were
    // missing their directories for the same reason.
    UICommon::CreateDirectories();
    UICommon::Init();
    impl->ui_initialized = true;
  }
  Config::SetBase(Config::MAIN_FULLSCREEN, impl->config.fullscreen);

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  // MODERNGEKKO_CPU_CORE=jit lets a benchmark run Dolphin's own dynamic
  // recompiler instead of the static one, which is the only way to answer
  // whether the recompiled module is actually beating the JIT. Diagnostic
  // only -- StaticRecomp remains the default and the shipped configuration.
  PowerPC::CPUCore cpu_core = PowerPC::CPUCore::StaticRecomp;
  if (const char *core_override = std::getenv("MODERNGEKKO_CPU_CORE")) {
    const std::string requested = core_override;
    if (requested == "jit" || requested == "jit64" || requested == "jitarm64")
#if defined(_M_ARM_64) || defined(__aarch64__)
      cpu_core = PowerPC::CPUCore::JITARM64;
#else
      cpu_core = PowerPC::CPUCore::JIT64;
#endif
    else if (requested == "interpreter")
      cpu_core = PowerPC::CPUCore::Interpreter;
    else if (requested == "cachedinterpreter")
      cpu_core = PowerPC::CPUCore::CachedInterpreter;
    std::fprintf(stderr, "[moderngekko] cpu core override: %s\n", core_override);
  }
  Config::SetBase(Config::MAIN_CPU_CORE, cpu_core);
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE,
                    *impl->config.graphics.internal_resolution_scale);
  Config::SetBase(Config::GFX_SHADER_CACHE, true);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (impl->config.headless) {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  } else if (impl->config.audio.backend.empty() ||
             !std::ranges::contains(audio_backends,
                                    impl->config.audio.backend)) {
    constexpr std::array preferred_backends = {
        BACKEND_CUBEB, BACKEND_PULSEAUDIO, BACKEND_ALSA};
    const auto preferred =
        std::ranges::find_if(preferred_backends, [&](const char *backend) {
          return std::ranges::contains(audio_backends, backend);
        });
    impl->config.audio.backend =
        preferred != preferred_backends.end() ? *preferred : BACKEND_NULLSOUND;
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                  impl->config.input.background_input);

  auto &jit = Core::System::GetInstance().GetJitInterface();
  StaticRecompModuleSource recomp_source;
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    recomp_source =
        StaticRecompModuleSource::Dynamic(impl->config.module.path.string());
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    recomp_source = StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor));
  if (!impl->mods->Empty()) {
    recomp_source.host_call = &ModManager::HostCall;
    recomp_source.host_call_contains = &ModManager::HostCallContains;
    recomp_source.host_call_range_contains =
        &ModManager::HostCallRangeContains;
    recomp_source.host_call_user = impl->mods.get();
  }
  jit.SetStaticRecompModuleSource(std::move(recomp_source));

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  s_status_file = impl->config.status_file;
  s_load_state_path = impl->config.load_state_path;
  s_save_state_path = impl->config.save_state_path;
  s_load_state_after_seconds = impl->config.load_state_after_seconds;
  s_save_state_after_seconds = impl->config.save_state_after_seconds;
  s_run_seconds = impl->config.run_seconds;
  if (const char* value = std::getenv("KART_SCREENSHOT_AFTER"))
    s_screenshot_after_seconds = std::strtod(value, nullptr);
  else
    s_screenshot_after_seconds = 0.0;
  if (const char* value = std::getenv("KART_SCREENSHOT_TRIGGER"))
    s_screenshot_trigger = value;
  else
    s_screenshot_trigger.clear();
  s_hold_buttons = impl->config.hold_buttons;
  s_spam_buttons = impl->config.spam_buttons;
  s_spam_period_seconds = impl->config.spam_period_seconds;
  s_spam_stop_seconds = impl->config.spam_stop_seconds;
  s_spam_pressed = false;
  s_spam_next_toggle = 0.0;
  s_spam_next_tick = 0;
  s_hold_applied = false;
  s_load_state_done = false;
  s_save_state_done = false;
  s_screenshot_done = false;
  s_automation_started = false;
  s_status_updates = 0;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_status_file.clear();
  s_load_state_path.clear();
  s_save_state_path.clear();
  s_run_seconds = 0.0;
  s_screenshot_after_seconds = 0.0;
  s_screenshot_trigger.clear();
  s_hold_buttons = 0;
  s_spam_buttons = 0;
  s_spam_pressed = false;
  s_spam_next_toggle = 0.0;
  s_hold_applied = false;
  s_automation_started = false;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          m_impl->metadata.main_dol.string(), std::move(*s_boot_session_data));
    else
      boot =
          BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
    s_boot_session_data.reset();
  }
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  std::jthread title_thread;
  std::jthread automation_thread;
  if (!s_status_file.empty() || !s_load_state_path.empty() ||
      !s_save_state_path.empty() || s_run_seconds > 0.0 || s_hold_buttons != 0 ||
      s_spam_buttons != 0) {
    automation_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        RunAutomationTick();
        for (int i = 0; i < 5 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
  }

  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        Host_UpdateTitle({});
        for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  m_impl->platform->MainLoop();
  automation_thread.request_stop();
  if (automation_thread.joinable())
    automation_thread.join();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko
