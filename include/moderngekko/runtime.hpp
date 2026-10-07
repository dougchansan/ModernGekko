#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moderngekko
{
struct ModuleSource
{
  enum class Kind
  {
    None,
    DynamicPath,
    AttachedDescriptor,
  };

  static ModuleSource DynamicPath(std::filesystem::path path);
  static ModuleSource AttachedDescriptor(const ModernGekkoModuleDesc* descriptor);

  Kind kind = Kind::None;
  std::filesystem::path path;
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

struct GraphicsSettings
{
  std::string backend;
  std::optional<int> internal_resolution_scale;
  bool widescreen_hack = false;
  bool native_ultrawide = false;
  int aspect_ratio = 0;
  int texture_filtering = 0;
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
};

struct QualityOfLifeSettings
{
  bool sixty_fps = false;
  // The task-list-aware guest idle skip. On by default: it is what lets
  // battle scenes hold 60. It is a trade, not a free win -- on a field
  // scene the task list is rarely empty, so each pass pays for the list
  // walk and then declines to idle, measured at about 8% there -- so a
  // machine short of headroom in the field may want it off.
  bool guest_idle_skip = true;
  // Dolphin dual core: the GPU thread renders and presents while the CPU
  // thread emulates, leaving the emulation thread headroom at 120/240 FPS.
  bool dual_core = true;
  bool fast_forward = false;
  int fast_forward_multiplier = 2;
  bool autosave = true;
  int autosave_slots = 5;
  bool crash_watchdog = true;
  int crash_watchdog_seconds = 15;
};

struct AudioSettings
{
  std::string backend;
};

struct InputSettings
{
  bool background_input = false;
};

struct AutomationSettings
{
  std::filesystem::path directory;
};

enum class WindowSystem
{
  Default,
  Wayland,
  X11,
};

struct RuntimeConfig
{
  std::filesystem::path game_root;
  std::filesystem::path user_directory;
  ModuleSource module;
  std::vector<std::filesystem::path> mod_directories;
  GraphicsSettings graphics;
  AudioSettings audio;
  InputSettings input;
  QualityOfLifeSettings qol;
  AutomationSettings automation;
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  bool fullscreen = false;
  bool allow_interpreter = false;
  bool show_fps_in_title = true;
  std::optional<std::string> window_title;
  // Boot straight into a savestate instead of from the title screen.
  std::optional<std::filesystem::path> load_state_path;
};

enum class RuntimeErrorCode
{
  AlreadyActive,
  InvalidGame,
  ModuleRequired,
  ModuleRejected,
  PlatformUnavailable,
  InitializationFailed,
  BootFailed,
  InvalidState,
};

struct RuntimeError
{
  RuntimeErrorCode code = RuntimeErrorCode::InitializationFailed;
  std::string message;
};

enum class RuntimeExitReason
{
  Stopped,
  BootFailed,
};

struct RuntimeRunResult
{
  RuntimeExitReason reason = RuntimeExitReason::Stopped;
  std::optional<RuntimeError> error;
};

class Runtime;

struct RuntimeCreateResult
{
  std::unique_ptr<Runtime> runtime;
  std::optional<RuntimeError> error;

  explicit operator bool() const { return runtime != nullptr; }
};

class Runtime final
{
public:
  static RuntimeCreateResult Create(RuntimeConfig config);

  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  RuntimeRunResult Run();
  void RequestStop();
  std::optional<RuntimeError> Pause();
  std::optional<RuntimeError> Resume();

  const RuntimeConfig& GetConfig() const;
  const GameMetadata& GetGameMetadata() const;
  const std::string& GetWindowTitle() const;

private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko

namespace ModernGekko = moderngekko;
