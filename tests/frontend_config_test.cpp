#include "frontend_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr const char *CONTROLLER_CONFIG_NAME = "GCPadNew.ini";
#else
constexpr const char *CONTROLLER_CONFIG_NAME = "WiimoteNew.ini";
#endif

int main() {
  namespace fs = std::filesystem;
  const fs::path directory =
      fs::temp_directory_path() /
      ("moderngekko-frontend-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string error;
  const std::string controller = "SDL/0/Test Controller";
  if (!moderngekko::frontend::SaveConfig(directory, "1920x1080", false,
                                         controller, &error))
    return 1;

  const auto loaded = moderngekko::frontend::LoadConfig(directory, false);
  if (!loaded || loaded.dolphin_scale != 3 || loaded.show_fps_in_title ||
      loaded.controller != controller ||
      loaded.graphics_backend != "Vulkan" || loaded.hires_textures ||
      loaded.community_hd_texture_pack ||
      loaded.text_upscale || loaded.sixty_fps ||
      loaded.aspect_ratio != moderngekko::frontend::kFrontendAspectAuto ||
      loaded.fast_forward) {
    return 2;
  }

  moderngekko::frontend::ConfigResult netplay_config = loaded;
  netplay_config.graphics_backend = "OpenGL";
  netplay_config.fullscreen = true;
  netplay_config.widescreen_hack = true;
  netplay_config.aspect_ratio = moderngekko::frontend::kFrontendAspectUltrawide;
  netplay_config.force_texture_filtering = 2;
  netplay_config.max_anisotropy = 4;
  netplay_config.osd_font_size = 24;
  netplay_config.navigation_overlay = 2;
  netplay_config.hires_textures = true;
  netplay_config.community_hd_texture_pack = true;
  netplay_config.cache_hires_textures = true;
  netplay_config.dump_textures = true;
  netplay_config.text_upscale = true;
  netplay_config.ultrawide_efb_scale = 4;
  netplay_config.input_overlay = true;
  netplay_config.minimap_high_contrast = true;
  netplay_config.accessibility_ui_scale = 150;
  netplay_config.sixty_fps = true;
  netplay_config.fast_forward = true;
  netplay_config.fast_forward_multiplier = 3;
  netplay_config.autosave = false;
  netplay_config.autosave_slots = 7;
  netplay_config.crash_watchdog = false;
  netplay_config.crash_watchdog_seconds = 30;
  netplay_config.controllers = {controller, "SDL/1/Second Controller"};
  netplay_config.controller = controller;
  netplay_config.netplay_nickname = "Kirby";
  netplay_config.netplay_address = "192.168.1.50";
  netplay_config.netplay_port = 34567;
  netplay_config.netplay_buffer = "auto";
  if (!moderngekko::frontend::SaveConfig(directory, netplay_config, &error))
    return 6;
  const auto netplay_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!netplay_loaded ||
      netplay_loaded.controllers != netplay_config.controllers ||
      netplay_loaded.netplay_nickname != "Kirby" ||
      netplay_loaded.netplay_address != "192.168.1.50" ||
      netplay_loaded.netplay_port != 34567 ||
      netplay_loaded.netplay_buffer != "auto" ||
      netplay_loaded.graphics_backend != "OGL" ||
      !netplay_loaded.widescreen_hack ||
      netplay_loaded.aspect_ratio !=
          moderngekko::frontend::kFrontendAspectUltrawide ||
      netplay_loaded.force_texture_filtering != 2 ||
      netplay_loaded.max_anisotropy != 4 ||
      netplay_loaded.osd_font_size != 24 ||
      netplay_loaded.navigation_overlay != 2 ||
      !netplay_loaded.hires_textures ||
      !netplay_loaded.community_hd_texture_pack ||
      !netplay_loaded.cache_hires_textures ||
      !netplay_loaded.dump_textures ||
      !netplay_loaded.text_upscale ||
      netplay_loaded.ultrawide_efb_scale != 4 ||
      !netplay_loaded.input_overlay ||
      !netplay_loaded.minimap_high_contrast ||
      netplay_loaded.accessibility_ui_scale != 150 ||
      !netplay_loaded.sixty_fps ||
      !netplay_loaded.fast_forward ||
      netplay_loaded.fast_forward_multiplier != 3 ||
      netplay_loaded.autosave || netplay_loaded.autosave_slots != 7 ||
      netplay_loaded.crash_watchdog ||
      netplay_loaded.crash_watchdog_seconds != 30 ||
      !netplay_loaded.fullscreen) {
    return 7;
  }

  auto metal_config = netplay_loaded;
  metal_config.graphics_backend = "Metal";
  metal_config.resolution = "3200x2640";
  if (!moderngekko::frontend::SaveConfig(directory, metal_config, &error))
    return 27;
  const auto metal_loaded = moderngekko::frontend::LoadConfig(directory, false);
  if (!metal_loaded || metal_loaded.graphics_backend != "Metal" ||
      metal_loaded.dolphin_scale != 5)
    return 28;

  auto invalid_netplay = netplay_config;
  invalid_netplay.netplay_address = "not a host";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 8;
  invalid_netplay = netplay_config;
  invalid_netplay.netplay_nickname = std::string(31, 'K');
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 9;
  invalid_netplay = netplay_config;
  invalid_netplay.graphics_backend = "Direct3D 9";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 13;
  invalid_netplay = netplay_config;
  invalid_netplay.aspect_ratio = 99;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 14;
  invalid_netplay = netplay_config;
  invalid_netplay.navigation_overlay = -1;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 15;
  invalid_netplay = netplay_config;
  invalid_netplay.ultrawide_efb_scale = 0;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 16;
  invalid_netplay = netplay_config;
  invalid_netplay.ultrawide_efb_scale = 2;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 17;
  invalid_netplay = netplay_config;
  invalid_netplay.accessibility_ui_scale = 201;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 19;
  invalid_netplay = netplay_config;
  invalid_netplay.fast_forward_multiplier = 5;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 20;
  invalid_netplay = netplay_config;
  invalid_netplay.autosave_slots = 0;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 21;
  invalid_netplay = netplay_config;
  invalid_netplay.crash_watchdog_seconds = 9;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 22;

  const fs::path legacy_directory = directory / "legacy-auto";
  fs::create_directories(legacy_directory);
  {
    std::ofstream legacy_config(legacy_directory / "config.ini");
    legacy_config << "[Video]\nresolution=1280x720\n"
                     "ultrawide_efb_scale=0\n";
  }
  const auto migrated_legacy =
      moderngekko::frontend::LoadConfig(legacy_directory, false);
  if (!migrated_legacy || migrated_legacy.ultrawide_efb_scale != 5)
    return 18;

  // A config written by a release that still had native_ultrawide must keep
  // loading. LoadConfig ignores unknown keys, and a load error is fatal, so a
  // regression here would stop every existing install from starting.
  const fs::path retired_directory = directory / "retired-key";
  fs::create_directories(retired_directory);
  {
    std::ofstream retired_config(retired_directory / "config.ini");
    retired_config << "[Video]\nresolution=1280x720\n"
                      "native_ultrawide=true\naspect_ratio=1\n";
  }
  const auto retired_loaded =
      moderngekko::frontend::LoadConfig(retired_directory, false);
  if (!retired_loaded ||
      retired_loaded.aspect_ratio !=
          moderngekko::frontend::kFrontendAspectWidescreen)
    return 23;

  // 4 and 5 were raw Dolphin Custom/CustomStretch modes the launcher never
  // offered. aspect_ratio is a frontend enum now, so retire them to Auto
  // rather than refusing to start.
  for (const auto &[value, code] :
       std::initializer_list<std::pair<const char *, int>>{{"4", 24}, {"5", 25}}) {
    const fs::path retired_aspect = directory / (std::string("retired-aspect-") + value);
    fs::create_directories(retired_aspect);
    {
      std::ofstream retired_config(retired_aspect / "config.ini");
      retired_config << "[Video]\nresolution=1280x720\naspect_ratio=" << value
                     << "\n";
    }
    const auto retired = moderngekko::frontend::LoadConfig(retired_aspect, false);
    if (!retired ||
        retired.aspect_ratio != moderngekko::frontend::kFrontendAspectAuto)
      return code;
  }

  // The frontend enum must not be handed to Dolphin unmapped.
  if (moderngekko::frontend::AspectModeForFrontendAspect(
          moderngekko::frontend::kFrontendAspectUltrawide) !=
          moderngekko::frontend::kFrontendAspectWidescreen ||
      moderngekko::frontend::AspectModeForFrontendAspect(
          moderngekko::frontend::kFrontendAspectStandard) !=
          moderngekko::frontend::kFrontendAspectStandard ||
      !moderngekko::frontend::IsUltrawideFrontendAspect(
          moderngekko::frontend::kFrontendAspectUltrawide) ||
      moderngekko::frontend::IsUltrawideFrontendAspect(
          moderngekko::frontend::kFrontendAspectAuto))
    return 26;
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, netplay_config.controllers, &error))
    return 3;
  if (moderngekko::frontend::ReadConfiguredController(directory) != controller)
    return 4;
  if (moderngekko::frontend::ReadConfiguredControllers(directory) !=
      netplay_config.controllers)
    return 10;

  // Scoped so the handle is closed before the cleanup below. Windows refuses
  // to delete a file that is still open, where POSIX allows it, so leaving
  // these open makes remove_all throw there and only there.
  std::string generated;
  {
    std::ifstream input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    generated.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  if (!generated.contains("Buttons/A = `Button A`\n") ||
      !generated.contains("Buttons/Z = `Shoulder R`\n") ||
      !generated.contains("Main Stick/Up = `Left Y+`\n") ||
      !generated.contains("C-Stick/Up = `Right Y+`\n") ||
      !generated.contains("Triggers/L-Analog = `Trigger L`\n") ||
      !generated.contains("Rumble/Motor = `Motor L` | `Motor R`\n") ||
      !generated.contains("[GCPad2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("[Wiimote") || generated.contains("[BalanceBoard]")) {
    return 5;
  }
#else
  if (!generated.contains("Buttons/A = `Shoulder L`\n") ||
      !generated.contains("Buttons/1 = `Button W`\n") ||
      !generated.contains("Buttons/2 = `Button S`\n") ||
      !generated.contains("Shake/X = `Trigger L`\n") ||
      !generated.contains("D-Pad/Up = `Pad N` | `Left Y+`\n") ||
      !generated.contains("D-Pad/Right = `Pad E` | `Left X+`\n") ||
      !generated.contains("Extension = None\n") ||
      !generated.contains("Options/Sideways Wiimote = True\n") ||
      !generated.contains("[Wiimote2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("Nunchuk/")) {
    return 5;
  }
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const std::string custom =
      "[GCPad1]\nDevice = SDL/9/Custom Controller\nButtons/A = Custom\n";
#else
  const std::string custom =
      "[Wiimote1]\nDevice = SDL/9/Custom Controller\nButtons/1 = Custom\n";
#endif
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
    output << custom;
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, netplay_config.controllers, &error))
    return 11;
  std::string preserved;
  {
    std::ifstream custom_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    preserved.assign(std::istreambuf_iterator<char>(custom_input),
                     std::istreambuf_iterator<char>());
  }
  if (preserved != custom || moderngekko::frontend::ReadConfiguredController(
                                 directory) != "SDL/9/Custom Controller")
    return 12;

  fs::remove_all(directory);
  return 0;
}
