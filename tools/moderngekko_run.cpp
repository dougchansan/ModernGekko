#include "cache_affinity.hpp"
#include "automation_protocol.hpp"
#include "frontend_config.hpp"
#include "dol_patch.hpp"
#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"
#include "netplay_session.hpp"

#include <SDL3/SDL.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// For the affinity/priority setup in main(). WIN32_LEAN_AND_MEAN keeps the
// winsock and GDI surface out of a translation unit that only needs the
// processor-topology and process calls.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
#ifndef MODERNGEKKO_RUNNER_NAME
#define MODERNGEKKO_RUNNER_NAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

volatile std::sig_atomic_t s_stop_requested = 0;

void HandleStopSignal(int) { s_stop_requested = 1; }

void Usage() {
  std::cerr << "usage: " MODERNGEKKO_RUNNER_NAME
               " [--game <extracted-root>] [--module <path>]\n"
               "       [--user-dir <path>] [--title <text>] [--load-state <path>]\n"
               "       [--graphics <backend>] [--audio <backend>]\n"
               "       [--load-state <sav>] [--automation-dir <path>]\n"
               "       [--mods <directory>] [--no-mods]\n"
               "       [--wayland] [-X11] [--headless] [--allow-interpreter]\n"
               "       [--netplay-host | --netplay-join <host>] "
               "[--netplay-port <port>]\n"
               "       [--nickname <name>] [--buffer <auto|1-20>] "
               "[--controller <device>]...\n"
               "       Automation protocol: drop key=value command files into "
               "<automation-dir>/commands.\n"
               "       Supported commands: pause, resume, stop, clear_pad, "
               "pad, pad_frames, save_state, load_state, screenshot.\n"
               "       pad files require command=pad, port=0..3, and optional "
               "a/b/x/y/z/start/dpad_*/l/r/l_analog/r_analog/main_x/main_y/c_x/c_y.\n"
               "       Status is written to status.txt.\n"
               "       With no --game, boots the path in "
               "<user-dir>/default-game.txt.\n";
}

std::filesystem::path
ReadDefaultGame(const std::filesystem::path &user_directory,
                const std::filesystem::path &executable_directory) {
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const std::filesystem::path default_file =
      executable_directory / "default-game.txt";
#else
  const std::filesystem::path default_file =
      user_directory / "default-game.txt";
#endif
  std::ifstream file(default_file);
  std::string path;
  std::getline(file, path);
  if (!path.empty() && path.back() == '\r')
    path.pop_back();
  std::filesystem::path result(path);
  if (result.is_relative())
    result = executable_directory / result;
  return result;
}

std::filesystem::path
DefaultUserDirectory(const std::filesystem::path &executable_directory) {
#ifdef MODERNGEKKO_PORTABLE_USER_DIRECTORY
  return executable_directory / "User";
#else
#ifdef MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
#if defined(_WIN32)
  if (const char *user_profile = std::getenv("USERPROFILE"))
    return std::filesystem::path(user_profile) / "Documents" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / "Documents" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
#if defined(_WIN32)
  if (const char *local_app_data = std::getenv("LOCALAPPDATA"))
    return std::filesystem::path(local_app_data) /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *xdg = std::getenv("XDG_DATA_HOME"))
    return std::filesystem::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".local" / "share" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
#endif
}

std::string LibrarySuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::filesystem::path ExecutableDirectory(const char *argv0) {
  std::error_code ec;
#if defined(__linux__)
  const std::filesystem::path proc_executable =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const std::filesystem::path executable =
      std::filesystem::weakly_canonical(argv0, ec);
  return ec ? std::filesystem::current_path() : executable.parent_path();
}

void AddOptionalMod(std::vector<std::filesystem::path> *directories,
                    const std::filesystem::path &executable_directory,
                    const char *relative_package, bool enabled) {
  if (!enabled || !relative_package || !*relative_package)
    return;
  const std::filesystem::path package = executable_directory / relative_package;
  std::error_code ec;
  if (std::filesystem::is_directory(package, ec))
    directories->push_back(package);
  else
    std::cerr << "optional mod package not found: " << package.string() << '\n';
}

// 21:9 is ~2.37 and 32:9 ~3.56, while 16:9 is 1.78. Anything at or past 2.0 is
// an ultrawide panel, where Colosseum's Hor+ camera is what "Auto" should mean.
constexpr double kUltrawideDisplayAspect = 2.0;

double DetectPrimaryDisplayAspect() {
  // The runner initialises SDL video later; only borrow the subsystem if it is
  // not up yet, and hand it back so that initialisation is unaffected.
  const bool already_initialised = SDL_WasInit(SDL_INIT_VIDEO) != 0;
  if (!already_initialised && !SDL_InitSubSystem(SDL_INIT_VIDEO))
    return 0.0;
  double aspect = 0.0;
  if (const SDL_DisplayID display = SDL_GetPrimaryDisplay()) {
    if (const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display)) {
      if (mode->w > 0 && mode->h > 0)
        aspect = static_cast<double>(mode->w) / static_cast<double>(mode->h);
    }
  }
  if (!already_initialised)
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return aspect;
}

bool ResolveNativeUltrawide(int frontend_aspect) {
  if (moderngekko::frontend::IsUltrawideFrontendAspect(frontend_aspect))
    return true;
  if (frontend_aspect != moderngekko::frontend::kFrontendAspectAuto)
    return false;
  const double display_aspect = DetectPrimaryDisplayAspect();
  const bool ultrawide = display_aspect >= kUltrawideDisplayAspect;
  if (ultrawide) {
    std::cerr << "auto aspect: display is " << display_aspect
              << ":1, enabling native ultrawide (Hor+)\n";
  }
  return ultrawide;
}
} // namespace

int RunMain(int argc, char **argv) {
#if defined(_WIN32)
  if (!std::getenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"))
    _putenv_s("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1");
#else
  setenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
#endif
  moderngekko::RuntimeConfig config;
  const std::filesystem::path executable_directory =
      ExecutableDirectory(argv[0]);
  config.user_directory = DefaultUserDirectory(executable_directory);
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  config.window_title = MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#endif
  std::filesystem::path module_path;
  bool use_default_mods = true;
  std::optional<moderngekko::frontend::NetplayRole> netplay_role;
  std::string netplay_address;
  std::optional<std::uint16_t> netplay_port;
  std::string netplay_nickname;
  std::string netplay_buffer;
  std::vector<std::string> netplay_controllers;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char *option) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << option << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--game")
      config.game_root = value("--game");
    else if (arg == "--module")
      module_path = value("--module");
    else if (arg == "--user-dir")
      config.user_directory = value("--user-dir");
    else if (arg == "--title")
      config.window_title = value("--title");
    else if (arg == "--load-state")
    {
      // Checked here rather than left to the boot path: a state that is not
      // there would otherwise boot to the title screen and look like it worked.
      std::filesystem::path state = value("--load-state");
      if (!std::filesystem::exists(state))
      {
        std::cerr << "savestate not found: " << state.string() << '\n';
        return 2;
      }
      config.load_state_path = std::move(state);
    }
    else if (arg == "--graphics")
      config.graphics.backend = value("--graphics");
    else if (arg == "--audio")
      config.audio.backend = value("--audio");
    else if (arg == "--load-state")
      config.load_state_path = value("--load-state");
    else if (arg == "--automation-dir")
      config.automation.directory = value("--automation-dir");
    else if (arg == "--mods")
      config.mod_directories.emplace_back(value("--mods"));
    else if (arg == "--no-mods")
      use_default_mods = false;
    else if (arg == "-X11" || arg == "--x11")
      config.window_system = moderngekko::WindowSystem::X11;
    else if (arg == "--wayland")
      config.window_system = moderngekko::WindowSystem::Wayland;
    else if (arg == "--headless")
      config.headless = true;
    else if (arg == "--allow-interpreter")
      config.allow_interpreter = true;
    else if (arg == "--netplay-host")
      netplay_role = moderngekko::frontend::NetplayRole::Host;
    else if (arg == "--netplay-join") {
      netplay_role = moderngekko::frontend::NetplayRole::Join;
      netplay_address = value("--netplay-join");
    } else if (arg == "--netplay-port") {
      const std::string port_value = value("--netplay-port");
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          port_value.data(), port_value.data() + port_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != port_value.data() + port_value.size() || port == 0 ||
          port > 65535) {
        std::cerr << "--netplay-port must be between 1 and 65535\n";
        return 2;
      }
      netplay_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--nickname")
      netplay_nickname = value("--nickname");
    else if (arg == "--buffer")
      netplay_buffer = value("--buffer");
    else if (arg == "--controller")
      netplay_controllers.emplace_back(value("--controller"));
    else if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      Usage();
      return 2;
    }
  }
  if (config.game_root.empty())
    config.game_root =
        ReadDefaultGame(config.user_directory, executable_directory);
  if (config.game_root.empty()) {
    std::cerr << "no game configured; use --game once or create "
              << (config.user_directory / "default-game.txt") << '\n';
    Usage();
    return 2;
  }

  auto frontend_config =
      moderngekko::frontend::LoadConfig(config.user_directory, true);
  if (!frontend_config) {
    std::cerr << "invalid config.ini: " << frontend_config.error << '\n';
    return 2;
  }
  config.graphics.internal_resolution_scale = frontend_config.dolphin_scale;
  config.graphics.widescreen_hack = frontend_config.widescreen_hack;
  // aspect_ratio is a frontend enum, not a Dolphin AspectMode: 32:9 is Hor+,
  // which the runtime drives through native_ultrawide plus a guest projection
  // multiplier. Split it into the two things the runtime actually consumes.
  const bool ultrawide_requested = ResolveNativeUltrawide(frontend_config.aspect_ratio);
  config.graphics.native_ultrawide = ultrawide_requested;
  config.graphics.aspect_ratio =
      moderngekko::frontend::AspectModeForFrontendAspect(frontend_config.aspect_ratio);
  config.graphics.texture_filtering =
      frontend_config.force_texture_filtering;
  config.graphics.max_anisotropy = frontend_config.max_anisotropy;
  config.graphics.osd_font_size = frontend_config.osd_font_size;
  config.graphics.navigation_overlay =
      moderngekko::automation::ResolveColosseumNavigationOverlayMode(
          frontend_config.navigation_overlay,
          frontend_config.minimap_high_contrast);
  config.graphics.hires_textures = frontend_config.hires_textures;
  config.graphics.community_hd_texture_pack =
      frontend_config.community_hd_texture_pack;
  config.graphics.cache_hires_textures =
      frontend_config.cache_hires_textures;
  config.graphics.dump_textures = frontend_config.dump_textures;
  config.graphics.text_upscale = frontend_config.text_upscale;
  config.graphics.ultrawide_efb_scale =
      frontend_config.ultrawide_efb_scale;
  config.graphics.input_overlay = frontend_config.input_overlay;
  config.graphics.minimap_high_contrast =
      frontend_config.minimap_high_contrast;
  config.graphics.accessibility_ui_scale =
      frontend_config.accessibility_ui_scale;
  config.qol.sixty_fps = frontend_config.sixty_fps;
  config.qol.guest_idle_skip = frontend_config.guest_idle_skip;
  config.qol.dual_core = frontend_config.dual_core;
  config.qol.fast_forward = frontend_config.fast_forward;
  config.qol.fast_forward_multiplier =
      frontend_config.fast_forward_multiplier;
  config.qol.autosave = frontend_config.autosave;
  config.qol.autosave_slots = frontend_config.autosave_slots;
  config.qol.crash_watchdog = frontend_config.crash_watchdog;
  config.qol.crash_watchdog_seconds =
      frontend_config.crash_watchdog_seconds;
  // The window title used to carry a hard-coded "[30 FPS]" suffix, because the
  // 60 FPS option was once delivered by a pre-patched module that targeted an
  // instruction the game never executes. It is now delivered by holding the
  // engine's frame period at 1 from the host each VI field (see the
  // vi_end_field hook in dolphin_runtime.cpp), which works, so the suffix was
  // asserting a frame rate that measurement contradicts and has been dropped.
  // The live rate is appended to the title at runtime when show_fps_in_title
  // is set; that is the only frame rate the title should claim.
  if (config.graphics.backend.empty())
    config.graphics.backend = frontend_config.graphics_backend;
  config.fullscreen = frontend_config.fullscreen;
  config.show_fps_in_title = frontend_config.show_fps_in_title;
  if (use_default_mods) {
    config.mod_directories.push_back(executable_directory / "Mods");
    config.mod_directories.push_back(config.user_directory / "Mods");
#ifdef MODERNGEKKO_NATIVE_ULTRAWIDE_MOD_PACKAGE
    AddOptionalMod(&config.mod_directories, executable_directory,
                   MODERNGEKKO_NATIVE_ULTRAWIDE_MOD_PACKAGE,
                   ultrawide_requested);
#endif
#ifdef MODERNGEKKO_TEXT_UPSCALE_MOD_PACKAGE
    AddOptionalMod(&config.mod_directories, executable_directory,
                   MODERNGEKKO_TEXT_UPSCALE_MOD_PACKAGE,
                   frontend_config.text_upscale);
#endif
#ifdef MODERNGEKKO_MINIMAP_MOD_PACKAGE
    AddOptionalMod(&config.mod_directories, executable_directory,
                   MODERNGEKKO_MINIMAP_MOD_PACKAGE,
                   frontend_config.minimap_high_contrast);
#endif
  }

  if (!netplay_role && !frontend_config.controller.empty()) {
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, frontend_config.controller,
            &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::cout << "controller configuration: " << controller_message << '\n';
  }

#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
  bool dol_changed = false;
  std::string dol_patch_error;
  if (!moderngekko::frontend::ApplyDolPatchManifest(
          config.game_root / "sys" / "main.dol",
          executable_directory / MODERNGEKKO_DOL_PATCH_MANIFEST, &dol_changed,
          &dol_patch_error)) {
    std::cerr << "DOL patching failed: " << dol_patch_error << '\n';
    return 2;
  }
  if (dol_changed)
    std::cout << "Applied native DOL patches\n";
#endif
  const auto inspected = moderngekko::InspectGame(config.game_root);
  if (!inspected) {
    std::cerr << "invalid game: " << inspected.error << '\n';
    return 2;
  }

#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (inspected.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID) {
    std::cerr << "unsupported disc ID: expected "
              << MODERNGEKKO_REQUIRED_DISC_ID << ", got "
              << inspected.metadata->disc_id << '\n';
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (inspected.metadata->dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256) {
    std::cerr << "unsupported main DOL: this release requires its pinned game build\n";
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (inspected.metadata->rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256) {
    std::cerr << "unsupported _Main.rel: this release requires its pinned game build\n";
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (inspected.metadata->assets_sha256 !=
      MODERNGEKKO_REQUIRED_ASSETS_SHA256) {
    std::cerr << "unsupported game assets: this release requires its pinned game build\n";
    return 2;
  }
#endif

  // Compatibility discovery belongs to the runner, never the runtime library.
  if (module_path.empty()) {
    if (const char *env = std::getenv("STATICRECOMP_MODULE"))
      module_path = env;
    else {
      // Static recompilation bakes guest instructions into the host module, so
      // an Action Replay instruction replacement cannot be switched after the
      // module is loaded. Setup therefore builds a stock module and a module
      // with the selected 60 FPS AR instruction pre-applied; the ordinary
      // config toggle chooses between them before boot.
      const std::string module_variant =
          frontend_config.sixty_fps ? "_recomp_60fps" : "_recomp";
      const std::string module_name = "g" + inspected.metadata->disc_id +
                                      module_variant + LibrarySuffix();
      const auto bundled = executable_directory / module_name;
      const auto user_module =
          config.user_directory / "StaticRecompModules" / module_name;
      if (std::filesystem::is_regular_file(bundled))
        module_path = bundled;
      else if (std::filesystem::is_regular_file(user_module))
        module_path = user_module;
      else
      {
        std::cerr << "The native game module is missing: " << module_name
                  << "\nDisc extraction completed, but the game has not been recompiled."
                  << "\nBuild the module from your own game with the repository setup "
                     "instructions, then place it in:\n"
                  << (config.user_directory / "StaticRecompModules").string() << '\n';
        return 2;
      }
    }
  }
  if (!module_path.empty())
    config.module =
        moderngekko::ModuleSource::DynamicPath(std::move(module_path));

#if defined(__linux__) || defined(_WIN32)
  if (!config.headless && config.graphics.backend.empty())
    config.graphics.backend = "Vulkan";
#endif

  if (netplay_role) {
    moderngekko::frontend::NetplayOptions options;
    options.role = *netplay_role;
    options.address = netplay_address.empty() ? frontend_config.netplay_address
                                              : netplay_address;
    options.port = netplay_port.value_or(frontend_config.netplay_port);
    options.nickname = netplay_nickname.empty()
                           ? frontend_config.netplay_nickname
                           : netplay_nickname;
    options.buffer = netplay_buffer.empty() ? frontend_config.netplay_buffer
                                            : netplay_buffer;
    const std::vector<std::string> configured_controllers =
        moderngekko::frontend::ReadConfiguredControllers(config.user_directory);
    options.controllers =
        netplay_controllers.empty()
            ? (configured_controllers.empty() ? frontend_config.controllers
                                              : configured_controllers)
            : netplay_controllers;
    if (options.controllers.empty() && !frontend_config.controller.empty())
      options.controllers.push_back(frontend_config.controller);
    if (options.controllers.empty()) {
      std::cerr << "netplay requires at least one selected controller\n";
      return 2;
    }
    frontend_config.netplay_address = options.address;
    frontend_config.netplay_port = options.port;
    frontend_config.netplay_nickname = options.nickname;
    frontend_config.netplay_buffer = options.buffer;
    frontend_config.controllers = options.controllers;
    frontend_config.controller = options.controllers.front();
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, options.controllers, &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::string save_error;
    if (!moderngekko::frontend::SaveConfig(config.user_directory,
                                           frontend_config, &save_error)) {
      std::cerr << "configuration: " << save_error << '\n';
      return 2;
    }
    std::cerr << "netplay: runner started\n";
    return moderngekko::frontend::RunNetplayLobby(
        std::move(config), std::move(frontend_config), std::move(options));
  }

  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created) {
    std::cerr << "initialization failed: " << created.error->message << '\n';
    return 1;
  }
  std::cout << "audio backend: " << created.runtime->GetConfig().audio.backend
            << '\n';

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  std::jthread signal_watcher([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (s_stop_requested) {
        s_stop_requested = 0;
        created.runtime->RequestStop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  const moderngekko::RuntimeRunResult result = created.runtime->Run();
  signal_watcher.request_stop();
  if (result.error) {
    std::cerr << "runtime failed: " << result.error->message << '\n';
    return 1;
  }
  return 0;
}

#if defined(_WIN32)
// Optionally confine the process to the cores that share the largest L3.
//
// The emulated CPU is a single serial instruction stream, so nothing here is
// about parallelism -- core usage stays at 1.00 either way. It is about cache
// residency. A recompiled module is one dispatch switch spanning the whole
// game, which lives or dies on staying in L3, and on a chip with 3D V-Cache on
// one CCD only, Windows migrating the thread between dies makes it repeatedly
// lose its working set. Measured on a 9950X3D: 55.41 -> 70.42 fps, +27%.
//
// This applies to every Dolphin thread, not only the emulated CPU thread, so it
// is opt-in. Set MODERNGEKKO_CACHE_AFFINITY=1 to enable it after benchmarking
// the target machine.
void PinProcessToLargestCache() {
  if (!moderngekko::frontend::AffinityEnabled(
          std::getenv("MODERNGEKKO_CACHE_AFFINITY")))
    return;

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
  if (length == 0)
    return;
  std::vector<char> buffer(length);
  if (!GetLogicalProcessorInformationEx(
          RelationCache,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
    return;

  const moderngekko::frontend::CacheDomain domain =
      moderngekko::frontend::LargestSharedCache(buffer.data(), length);
  if (!domain)
    return;

  if (moderngekko::frontend::ApplyCacheDomain(GetCurrentProcess(), domain).affinity_set) {
    std::cout << "[perf] pinned to the cores sharing the largest L3 (" << (domain.size >> 20)
              << " MB), mask 0x" << std::hex << static_cast<unsigned long long>(domain.mask)
              << std::dec << '\n';
  }
}
#endif

int main(int argc, char **argv) {
  try {
#if defined(_WIN32)
    // MODERNGEKKO_WAIT_PID: set by the pause menu's Graphics API switch. Let the
    // previous session finish shutting down (memory card, config writes) first.
    if (const char *wait_pid = std::getenv("MODERNGEKKO_WAIT_PID"); wait_pid && *wait_pid) {
      if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE,
                                        static_cast<DWORD>(std::strtoul(wait_pid, nullptr, 10)))) {
        WaitForSingleObject(previous, 20000);
        CloseHandle(previous);
      }
      SetEnvironmentVariableA("MODERNGEKKO_WAIT_PID", nullptr);
    }
    PinProcessToLargestCache();
#endif
    return RunMain(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "fatal error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "fatal error: unknown exception\n";
  }
  return 1;
}
