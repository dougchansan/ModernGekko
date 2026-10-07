#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend {
struct ResolutionOption {
  const char *text;
  int dolphin_scale;
};

struct GraphicsBackendOption {
  const char *text;
  const char *value;
};

// Frontend aspect selection. Deliberately NOT a Dolphin AspectMode: 32:9 is
// Hor+, which Dolphin expresses as Stretch plus a guest projection multiplier
// rather than as an aspect mode, so there is no mode to map it onto.
// moderngekko_run translates this enum at the boundary.
enum FrontendAspect : int {
  kFrontendAspectAuto = 0,
  kFrontendAspectWidescreen = 1,
  kFrontendAspectStandard = 2,
  kFrontendAspectUltrawide = 3,
};
inline constexpr int kFrontendAspectMax = kFrontendAspectUltrawide;

// Dolphin AspectMode: 0 Auto, 1 ForceWide, 2 ForceStandard. Ultrawide reports
// ForceWide because the runtime overrides the mode once Hor+ gameplay starts;
// this value only governs the frames before that, where 16:9 beats Stretch.
constexpr int AspectModeForFrontendAspect(int aspect) {
  return aspect == kFrontendAspectUltrawide ? kFrontendAspectWidescreen : aspect;
}

constexpr bool IsUltrawideFrontendAspect(int aspect) {
  return aspect == kFrontendAspectUltrawide;
}

struct ConfigResult {
  int dolphin_scale = 0;
  bool widescreen_hack = false;
  int aspect_ratio = 0;
  int force_texture_filtering = 0;
  int max_anisotropy = -1;
  int osd_font_size = 13;
  int navigation_overlay = 0;
  bool hires_textures = false;
  bool community_hd_texture_pack = false;
  bool cache_hires_textures = false;
  bool dump_textures = false;
  bool text_upscale = false;
  int ultrawide_efb_scale = 5;
  bool input_overlay = false;
  bool minimap_high_contrast = false;
  int accessibility_ui_scale = 100;
  bool sixty_fps = false;
  bool guest_idle_skip = true;
  bool fast_forward = false;
  int fast_forward_multiplier = 2;
  bool autosave = true;
  int autosave_slots = 5;
  bool crash_watchdog = true;
  int crash_watchdog_seconds = 15;
  std::string resolution;
  std::string graphics_backend = "Vulkan";
  std::string controller;
  std::vector<std::string> controllers;
  bool show_fps_in_title = true;
  bool fullscreen = false;
  std::string netplay_nickname = "Player";
  std::string netplay_address = "127.0.0.1";
  std::uint16_t netplay_port = 2626;
  std::string netplay_buffer = "auto";
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

const std::vector<ResolutionOption> &SupportedResolutions();
const std::vector<GraphicsBackendOption> &SupportedGraphicsBackends();
ConfigResult LoadConfig(const std::filesystem::path &user_directory,
                        bool create_if_missing);
bool SaveConfig(const std::filesystem::path &user_directory,
                const ConfigResult &config, std::string *error);
bool SaveConfig(const std::filesystem::path &user_directory,
                std::string_view resolution, bool show_fps_in_title,
                std::string_view controller, std::string *error);
std::string
ReadConfiguredController(const std::filesystem::path &user_directory);
std::vector<std::string>
ReadConfiguredControllers(const std::filesystem::path &user_directory);
bool ControllerConfigExists(const std::filesystem::path &user_directory);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::string_view controller,
                              std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::string_view controller, std::string *message);
} // namespace moderngekko::frontend
