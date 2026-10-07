#include "frontend_config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

namespace moderngekko::frontend {
namespace {
std::string Trim(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ValidNetplayAddress(std::string_view value) {
  if (value.empty() || value.size() > 253)
    return false;
  return std::ranges::all_of(value, [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_';
  });
}

std::string NormalizeGraphicsBackend(std::string value) {
  const std::string lower = Lower(Trim(std::move(value)));
  if (lower == "vulkan")
    return "Vulkan";
  if (lower == "opengl" || lower == "ogl")
    return "OGL";
  if (lower == "metal")
    return "Metal";
#ifdef _WIN32
  // DXGI flip-model swap chains are what AMD Fluid Motion Frames hooks most
  // reliably on Windows.
  if (lower == "d3d12" || lower == "direct3d12" || lower == "dx12")
    return "D3D12";
  if (lower == "d3d11" || lower == "d3d" || lower == "direct3d11" || lower == "dx11")
    return "D3D";
#endif
  return {};
}

bool ParseBoolean(const std::string &value, bool *result) {
  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    *result = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    *result = false;
    return true;
  }
  return false;
}

bool ParseIntValue(const std::string &value, int *result, int min, int max) {
  int parsed = 0;
  const auto parsed_result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (parsed_result.ec != std::errc{} ||
      parsed_result.ptr != value.data() + value.size())
    return false;
  if (parsed < min || parsed > max)
    return false;
  *result = parsed;
  return true;
}

bool ValidUltrawideEfbScale(int scale) {
  return scale >= 3 && scale <= 6;
}

fs::path ControllerConfigPath(const fs::path &user_directory) {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  return user_directory / "Config" / "GCPadNew.ini";
#else
  return user_directory / "Config" / "WiimoteNew.ini";
#endif
}

std::string_view ControllerSectionPrefix() {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  return "[GCPad";
#else
  return "[Wiimote";
#endif
}

} // namespace

const std::vector<ResolutionOption> &SupportedResolutions() {
  // These are the output-resolution labels used by Dolphin's integer EFB
  // scales.
  static const std::vector<ResolutionOption> resolutions = {
      {"640x528", 1},   {"1280x720", 2},  {"1920x1080", 3},  {"2560x1440", 4},
      {"3200x2640", 5}, {"3840x2160", 6}, {"5120x2880", 8}, {"7680x4320", 12},
  };
  return resolutions;
}

const std::vector<GraphicsBackendOption> &SupportedGraphicsBackends() {
  static const std::vector<GraphicsBackendOption> backends = {
#ifdef __APPLE__
      {"Metal", "Metal"},
#endif
      {"Vulkan", "Vulkan"},
      {"OpenGL", "OGL"},
  };
  return backends;
}

ConfigResult LoadConfig(const fs::path &user_directory,
                        bool create_if_missing) {
  const fs::path path = user_directory / "config.ini";
  if (!fs::exists(path) && create_if_missing) {
    std::string error;
    if (!SaveConfig(user_directory, "1920x1080", true, {}, &error))
      return {.error = std::move(error)};
  }

  std::ifstream file(path);
  if (!file)
    return {.error = "can't open " + path.string()};

  ConfigResult config;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' ||
        trimmed[0] == '[')
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos)
      return {.error = "invalid config.ini line: " + trimmed};
    const std::string key = Lower(Trim(trimmed.substr(0, separator)));
    const std::string raw_value = Trim(trimmed.substr(separator + 1));
    const std::string value = Lower(raw_value);
    if (key == "resolution")
      config.resolution = value;
    else if (key == "backend" || key == "graphics_backend") {
      config.graphics_backend = NormalizeGraphicsBackend(raw_value);
      if (config.graphics_backend.empty())
        return {.error = "graphics backend must be Vulkan or OpenGL"};
    }
    else if (key == "controller")
      config.controller = raw_value;
    else if (key.starts_with("controller") && key.size() == 11 &&
             key.back() >= '1' && key.back() <= '4') {
      const std::size_t index = static_cast<std::size_t>(key.back() - '1');
      if (config.controllers.size() <= index)
        config.controllers.resize(index + 1);
      config.controllers[index] = raw_value;
    } else if (key == "show_fps_in_title") {
      if (!ParseBoolean(value, &config.show_fps_in_title))
        return {.error = "show_fps_in_title must be true or false"};
    } else if (key == "widescreen_hack") {
      if (!ParseBoolean(value, &config.widescreen_hack))
        return {.error = "widescreen_hack must be true or false"};
    } else if (key == "aspect_ratio") {
      // Parse the retired range and migrate rather than refusing to start: a
      // load error is fatal, and 4/5 were legal raw Dolphin AspectMode values
      // (Custom/CustomStretch) in earlier releases even though the launcher
      // never offered them. 3 now means 32:9 Hor+, not Stretch.
      if (!ParseIntValue(value, &config.aspect_ratio, 0, 5))
        return {.error = "aspect_ratio must be in range 0 to 3"};
      if (config.aspect_ratio > kFrontendAspectMax)
        config.aspect_ratio = kFrontendAspectAuto;
    } else if (key == "force_texture_filtering") {
      if (!ParseIntValue(value, &config.force_texture_filtering, 0, 2))
        return {.error = "force_texture_filtering must be in range 0 to 2"};
    } else if (key == "max_anisotropy") {
      if (!ParseIntValue(value, &config.max_anisotropy, -1, 4))
        return {.error = "max_anisotropy must be in range -1 to 4"};
    } else if (key == "osd_font_size") {
      if (!ParseIntValue(value, &config.osd_font_size, 8, 48))
        return {.error = "osd_font_size must be in range 8 to 48"};
    } else if (key == "navigation_overlay") {
      if (!ParseIntValue(value, &config.navigation_overlay, 0, 2))
        return {.error = "navigation_overlay must be in range 0 to 2"};
    } else if (key == "hires_textures") {
      if (!ParseBoolean(value, &config.hires_textures))
        return {.error = "hires_textures must be true or false"};
    } else if (key == "community_hd_texture_pack") {
      if (!ParseBoolean(value, &config.community_hd_texture_pack))
        return {.error = "community_hd_texture_pack must be true or false"};
    } else if (key == "cache_hires_textures") {
      if (!ParseBoolean(value, &config.cache_hires_textures))
        return {.error = "cache_hires_textures must be true or false"};
    } else if (key == "dump_textures") {
      if (!ParseBoolean(value, &config.dump_textures))
        return {.error = "dump_textures must be true or false"};
    } else if (key == "text_upscale") {
      if (!ParseBoolean(value, &config.text_upscale))
        return {.error = "text_upscale must be true or false"};
    } else if (key == "input_overlay") {
      if (!ParseBoolean(value, &config.input_overlay))
        return {.error = "input_overlay must be true or false"};
    } else if (key == "minimap_high_contrast") {
      if (!ParseBoolean(value, &config.minimap_high_contrast))
        return {.error = "minimap_high_contrast must be true or false"};
    } else if (key == "accessibility_ui_scale") {
      if (!ParseIntValue(value, &config.accessibility_ui_scale, 75, 200))
        return {.error = "accessibility_ui_scale must be from 75 to 200"};
    } else if (key == "sixty_fps") {
      if (!ParseBoolean(value, &config.sixty_fps))
        return {.error = "sixty_fps must be true or false"};
    } else if (key == "guest_idle_skip") {
      if (!ParseBoolean(value, &config.guest_idle_skip))
        return {.error = "guest_idle_skip must be true or false"};
    } else if (key == "fast_forward") {
      if (!ParseBoolean(value, &config.fast_forward))
        return {.error = "fast_forward must be true or false"};
    } else if (key == "fast_forward_multiplier") {
      if (!ParseIntValue(value, &config.fast_forward_multiplier, 2, 4))
        return {.error = "fast_forward_multiplier must be from 2 to 4"};
    } else if (key == "autosave") {
      if (!ParseBoolean(value, &config.autosave))
        return {.error = "autosave must be true or false"};
    } else if (key == "autosave_slots") {
      if (!ParseIntValue(value, &config.autosave_slots, 1, 10))
        return {.error = "autosave_slots must be from 1 to 10"};
    } else if (key == "crash_watchdog") {
      if (!ParseBoolean(value, &config.crash_watchdog))
        return {.error = "crash_watchdog must be true or false"};
    } else if (key == "crash_watchdog_seconds") {
      if (!ParseIntValue(value, &config.crash_watchdog_seconds, 10, 60))
        return {.error = "crash_watchdog_seconds must be from 10 to 60"};
    } else if (key == "ultrawide_efb_scale") {
      if (!ParseIntValue(value, &config.ultrawide_efb_scale, 0, 12))
        return {.error = "ultrawide_efb_scale must be from 3 to 6"};
      // Migrate the retired aspect-driven Auto mode to the stable quality
      // preset. Auto could allocate an excessive EFB at 32:9 and stall play.
      if (config.ultrawide_efb_scale == 0)
        config.ultrawide_efb_scale = 5;
      else if (!ValidUltrawideEfbScale(config.ultrawide_efb_scale))
        return {.error = "ultrawide_efb_scale must be from 3 to 6"};
    } else if (key == "fullscreen") {
      if (!ParseBoolean(value, &config.fullscreen))
        return {.error = "fullscreen must be true or false"};
    } else if (key == "nickname")
      config.netplay_nickname = raw_value;
    else if (key == "address")
      config.netplay_address = raw_value;
    else if (key == "port") {
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          raw_value.data(), raw_value.data() + raw_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != raw_value.data() + raw_value.size() || port == 0 ||
          port > 65535)
        return {.error = "netplay port must be between 1 and 65535"};
      config.netplay_port = static_cast<std::uint16_t>(port);
    } else if (key == "buffer") {
      if (value != "auto") {
        unsigned int frames = 0;
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), frames);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || frames < 1 ||
            frames > 20)
          return {.error =
                      "netplay buffer must be auto or a value from 1 to 20"};
      }
      config.netplay_buffer = value;
    }
  }
  if (config.resolution.empty())
    return {.error = "config.ini is missing resolution=<width>x<height>"};

  std::erase(config.controllers, std::string{});
  if (config.controllers.empty() && !config.controller.empty())
    config.controllers.push_back(config.controller);
  if (config.controller.empty() && !config.controllers.empty())
    config.controller = config.controllers.front();
  if (config.netplay_nickname.empty())
    return {.error = "netplay nickname cannot be empty"};
  if (config.netplay_nickname.size() > 30)
    return {.error = "netplay nickname cannot exceed 30 characters"};
  if (!ValidNetplayAddress(config.netplay_address))
    return {.error = "netplay address must be an IPv4 address or hostname"};

  for (const ResolutionOption &option : SupportedResolutions()) {
    if (config.resolution == option.text) {
      config.dolphin_scale = option.dolphin_scale;
      return config;
    }
  }

  // Dolphin also accepts exact raw EFB multiples even when they do not have a
  // common display label.
  for (int scale = 1; scale <= 12; ++scale) {
    const std::string raw =
        std::to_string(640 * scale) + "x" + std::to_string(528 * scale);
    if (config.resolution == raw) {
      config.dolphin_scale = scale;
      return config;
    }
  }

  return {.error = "unsupported Dolphin internal resolution '" +
                   config.resolution +
                   "'; use a listed display resolution or an exact 640x528 "
                   "multiple up to 12x"};
}

bool SaveConfig(const fs::path &user_directory, const ConfigResult &config,
                std::string *error) {
  const std::string graphics_backend =
      NormalizeGraphicsBackend(config.graphics_backend);
  if (config.resolution.empty() || graphics_backend.empty() ||
      config.aspect_ratio < 0 || config.aspect_ratio > kFrontendAspectMax ||
      config.force_texture_filtering < 0 ||
      config.force_texture_filtering > 2 ||
      config.max_anisotropy < -1 || config.max_anisotropy > 4 ||
      config.osd_font_size < 8 || config.osd_font_size > 48 ||
      config.navigation_overlay < 0 || config.navigation_overlay > 2 ||
      config.accessibility_ui_scale < 75 ||
      config.accessibility_ui_scale > 200 ||
      config.fast_forward_multiplier < 2 ||
      config.fast_forward_multiplier > 4 || config.autosave_slots < 1 ||
      config.autosave_slots > 10 || config.crash_watchdog_seconds < 10 ||
      config.crash_watchdog_seconds > 60 ||
      !ValidUltrawideEfbScale(config.ultrawide_efb_scale) ||
      config.netplay_nickname.empty() ||
      config.netplay_nickname.size() > 30 ||
      config.netplay_nickname.find_first_of("\r\n") != std::string::npos ||
      !ValidNetplayAddress(config.netplay_address) ||
      config.netplay_address.find_first_of("\r\n") != std::string::npos ||
      config.netplay_port == 0) {
    if (error)
      *error = "invalid frontend settings";
    return false;
  }
  if (config.netplay_buffer != "auto") {
    unsigned int frames = 0;
    const auto parsed = std::from_chars(
        config.netplay_buffer.data(),
        config.netplay_buffer.data() + config.netplay_buffer.size(), frames);
    if (parsed.ec != std::errc{} ||
        parsed.ptr !=
            config.netplay_buffer.data() + config.netplay_buffer.size() ||
        frames < 1 || frames > 20) {
      if (error)
        *error = "netplay buffer must be auto or a value from 1 to 20";
      return false;
    }
  }
  std::error_code ec;
  fs::create_directories(user_directory, ec);
  if (ec) {
    if (error)
      *error = "can't create user directory: " + ec.message();
    return false;
  }
  std::ofstream file(user_directory / "config.ini", std::ios::trunc);
  if (!file) {
    if (error)
      *error = "can't write " + (user_directory / "config.ini").string();
    return false;
  }
  file << "# ModernGekko frontend settings\n"
          "# This is Dolphin's internal render target, not the window size.\n"
          "[Video]\n"
          "resolution="
       << config.resolution << '\n'
        << "backend=" << graphics_backend << '\n'
        << "widescreen_hack=" << (config.widescreen_hack ? "true" : "false") << '\n'
        << "aspect_ratio=" << config.aspect_ratio << '\n'
        << "force_texture_filtering=" << config.force_texture_filtering << '\n'
        << "max_anisotropy=" << config.max_anisotropy << '\n'
        << "osd_font_size=" << config.osd_font_size << '\n'
        << "navigation_overlay=" << config.navigation_overlay << '\n'
        << "hires_textures=" << (config.hires_textures ? "true" : "false") << '\n'
        << "community_hd_texture_pack="
        << (config.community_hd_texture_pack ? "true" : "false") << '\n'
        << "cache_hires_textures="
        << (config.cache_hires_textures ? "true" : "false") << '\n'
        << "dump_textures=" << (config.dump_textures ? "true" : "false") << '\n'
        << "text_upscale=" << (config.text_upscale ? "true" : "false") << '\n'
        << "input_overlay=" << (config.input_overlay ? "true" : "false") << '\n'
        << "minimap_high_contrast="
        << (config.minimap_high_contrast ? "true" : "false") << '\n'
        << "accessibility_ui_scale=" << config.accessibility_ui_scale << '\n'
        << "ultrawide_efb_scale=" << config.ultrawide_efb_scale << '\n'
       << "fullscreen=" << (config.fullscreen ? "true" : "false") << '\n'
       << "show_fps_in_title=" << (config.show_fps_in_title ? "true" : "false")
       << '\n'
       << "[QualityOfLife]\n"
       << "sixty_fps=" << (config.sixty_fps ? "true" : "false") << '\n'
       << "guest_idle_skip="
       << (config.guest_idle_skip ? "true" : "false") << '\n'
       << "fast_forward=" << (config.fast_forward ? "true" : "false") << '\n'
       << "fast_forward_multiplier=" << config.fast_forward_multiplier << '\n'
       << "autosave=" << (config.autosave ? "true" : "false") << '\n'
       << "autosave_slots=" << config.autosave_slots << '\n'
       << "crash_watchdog=" << (config.crash_watchdog ? "true" : "false") << '\n'
       << "crash_watchdog_seconds=" << config.crash_watchdog_seconds << '\n'
       << "[Input]\n";
  for (std::size_t i = 0; i < config.controllers.size() && i < 4; ++i) {
    if (config.controllers[i].find_first_of("\r\n") != std::string::npos) {
      if (error)
        *error = "controller device cannot contain a newline";
      return false;
    }
    file << "controller" << i + 1 << '=' << config.controllers[i] << '\n';
  }
  file << "[Netplay]\n"
       << "nickname=" << config.netplay_nickname << '\n'
       << "address=" << config.netplay_address << '\n'
       << "port=" << config.netplay_port << '\n'
       << "buffer=" << config.netplay_buffer << '\n';
  return true;
}

bool SaveConfig(const fs::path &user_directory, std::string_view resolution,
                bool show_fps_in_title, std::string_view controller,
                std::string *error) {
  ConfigResult config = LoadConfig(user_directory, false);
  if (!config)
    config = {};
  config.resolution = resolution;
  config.show_fps_in_title = show_fps_in_title;
  config.controller = controller;
  config.controllers.clear();
  if (!controller.empty())
    config.controllers.emplace_back(controller);
  return SaveConfig(user_directory, config, error);
}

std::string ReadConfiguredController(const fs::path &user_directory) {
  const std::vector<std::string> controllers =
      ReadConfiguredControllers(user_directory);
  return controllers.empty() ? std::string{} : controllers.front();
}

std::vector<std::string>
ReadConfiguredControllers(const fs::path &user_directory) {
  std::ifstream input(ControllerConfigPath(user_directory));
  std::vector<std::string> controllers;
  std::string line;
  std::size_t controller_index = 4;
  const std::string_view section_prefix = ControllerSectionPrefix();
  while (std::getline(input, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
      controller_index = 4;
      if (trimmed.size() == section_prefix.size() + 2 &&
          trimmed.starts_with(section_prefix) &&
          trimmed[section_prefix.size()] >= '1' &&
          trimmed[section_prefix.size()] <= '4')
        controller_index = static_cast<std::size_t>(
            trimmed[section_prefix.size()] - '1');
      continue;
    }
    if (controller_index >= 4)
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator != std::string::npos &&
        Trim(trimmed.substr(0, separator)) == "Device") {
      const std::string device = Trim(trimmed.substr(separator + 1));
      if (!device.empty()) {
        if (controllers.size() <= controller_index)
          controllers.resize(controller_index + 1);
        controllers[controller_index] = device;
      }
    }
  }
  std::erase(controllers, std::string{});
  return controllers;
}

bool ControllerConfigExists(const fs::path &user_directory) {
  std::error_code ec;
  return fs::is_regular_file(ControllerConfigPath(user_directory), ec);
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message) {
  if (controllers.empty() || controllers.size() > 4) {
    if (message)
      *message = "select between one and four connected SDL gamepads";
    return false;
  }
  for (const std::string &controller : controllers) {
    if (controller.empty() ||
        controller.find_first_of("\r\n") != std::string_view::npos) {
      if (message)
        *message = "select connected SDL gamepads";
      return false;
    }
  }

  const fs::path destination = ControllerConfigPath(user_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i) {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad" << i + 1 << "]\n";
    if (i >= controllers.size())
      continue;
    output << "Device = " << controllers[i] << '\n'
           << "Buttons/A = `Button A`\n"
              "Buttons/B = `Button B`\n"
              "Buttons/X = `Button X`\n"
              "Buttons/Y = `Button Y`\n"
              "Buttons/Z = `Shoulder R`\n"
              "Buttons/Start = Start\n"
              "Main Stick/Up = `Left Y+`\n"
              "Main Stick/Down = `Left Y-`\n"
              "Main Stick/Left = `Left X-`\n"
              "Main Stick/Right = `Left X+`\n"
              "Main Stick/Calibration = 100.00\n"
              "C-Stick/Up = `Right Y+`\n"
              "C-Stick/Down = `Right Y-`\n"
              "C-Stick/Left = `Right X-`\n"
              "C-Stick/Right = `Right X+`\n"
              "C-Stick/Calibration = 100.00\n"
              "Triggers/L = `Trigger L`\n"
              "Triggers/R = `Trigger R`\n"
              "Triggers/L-Analog = `Trigger L`\n"
              "Triggers/R-Analog = `Trigger R`\n"
              "D-Pad/Up = `Pad N`\n"
              "D-Pad/Down = `Pad S`\n"
              "D-Pad/Left = `Pad W`\n"
              "D-Pad/Right = `Pad E`\n"
              "Rumble/Motor = `Motor L` | `Motor R`\n";
#else
    output << "[Wiimote" << i + 1 << "]\n";
    if (i >= controllers.size())
      continue;
    output << "Device = " << controllers[i] << '\n'
           << "Buttons/A = `Shoulder L`\n"
              "Buttons/B = `Shoulder R`\n"
              "Buttons/1 = `Button W`\n"
              "Buttons/2 = `Button S`\n"
              "Buttons/- = Back\n"
              "Buttons/+ = Start\n"
              "Buttons/Home = Guide\n"
              "D-Pad/Up = `Pad N` | `Left Y+`\n"
              "D-Pad/Down = `Pad S` | `Left Y-`\n"
              "D-Pad/Left = `Pad W` | `Left X-`\n"
              "D-Pad/Right = `Pad E` | `Left X+`\n"
              "IR/Up = `Cursor Y-`\n"
              "IR/Down = `Cursor Y+`\n"
              "IR/Left = `Cursor X-`\n"
              "IR/Right = `Cursor X+`\n"
              "Shake/X = `Trigger L`\n"
              "Shake/Y = `Trigger R`\n"
              "Shake/Z = `Trigger L`\n"
              "IRPassthrough/Object 1 X = `IR Object 1 X`\n"
              "IRPassthrough/Object 1 Y = `IR Object 1 Y`\n"
              "IRPassthrough/Object 1 Size = `IR Object 1 Size`\n"
              "IRPassthrough/Object 2 X = `IR Object 2 X`\n"
              "IRPassthrough/Object 2 Y = `IR Object 2 Y`\n"
              "IRPassthrough/Object 2 Size = `IR Object 2 Size`\n"
              "IRPassthrough/Object 3 X = `IR Object 3 X`\n"
              "IRPassthrough/Object 3 Y = `IR Object 3 Y`\n"
              "IRPassthrough/Object 3 Size = `IR Object 3 Size`\n"
              "IRPassthrough/Object 4 X = `IR Object 4 X`\n"
              "IRPassthrough/Object 4 Y = `IR Object 4 Y`\n"
              "IRPassthrough/Object 4 Size = `IR Object 4 Size`\n"
              "IMUAccelerometer/Up = `Accel Up`\n"
              "IMUAccelerometer/Down = `Accel Down`\n"
              "IMUAccelerometer/Left = `Accel Left`\n"
              "IMUAccelerometer/Right = `Accel Right`\n"
              "IMUAccelerometer/Forward = `Accel Forward`\n"
              "IMUAccelerometer/Backward = `Accel Backward`\n"
              "IMUGyroscope/Pitch Up = `Gyro Pitch Up`\n"
              "IMUGyroscope/Pitch Down = `Gyro Pitch Down`\n"
              "IMUGyroscope/Roll Left = `Gyro Roll Left`\n"
              "IMUGyroscope/Roll Right = `Gyro Roll Right`\n"
              "IMUGyroscope/Yaw Left = `Gyro Yaw Left`\n"
              "IMUGyroscope/Yaw Right = `Gyro Yaw Right`\n"
              "Rumble/Motor = Motor\n"
              "Extension = None\n"
              "Options/Sideways Wiimote = True\n";
#endif
  }
#ifndef MODERNGEKKO_GAMECUBE_CONTROLLERS
  output << "[BalanceBoard]\n";
#endif
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  if (message)
    *message = std::to_string(controllers.size()) +
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
               " GameCube controller" +
#else
               " sideways Wii Remote" +
#endif
               (controllers.size() == 1 ? " mapped" : "s mapped");
  return true;
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::string_view controller,
                              std::string *message) {
  const std::string value(controller);
  return GenerateControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message) {
  if (ControllerConfigExists(user_directory)) {
    if (message)
      *message = "using existing controller profile";
    return true;
  }
  return GenerateControllerConfig(user_directory, controllers, message);
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::string_view controller, std::string *message) {
  const std::string value(controller);
  return EnsureControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}
} // namespace moderngekko::frontend
