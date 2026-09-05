#pragma once

#include "moderngekko/diagnostics.hpp"
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
};

struct AudioSettings
{
  std::string backend;
};

struct InputSettings
{
  bool background_input = false;
};

// Everything the runner needs to configure the diagnostics subsystem. When
// `enabled` is false the runtime never initializes diagnostics at all.
struct DiagnosticsSettings
{
  bool enabled = false;
  diagnostics::Level level = diagnostics::Level::Basic;
  bool overlay = false;
  bool anonymize = true;
  // Start capturing as soon as the game boots and stop after this many
  // seconds. 0 means "wait for the capture hotkey".
  double capture_seconds = 0.0;
  bool capture_on_boot = false;
  double history_seconds = 30.0;
  unsigned sample_hz = 500;
  std::filesystem::path output_directory;
  std::filesystem::path symbol_file;
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
  DiagnosticsSettings diagnostics;
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  bool fullscreen = false;
  bool allow_interpreter = false;
  bool show_fps_in_title = true;
  std::optional<std::string> window_title;
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

  // Diagnostics capture control. All of these are no-ops that return false
  // when diagnostics were not enabled for this runtime.
  bool IsDiagnosticsEnabled() const;
  bool IsDiagnosticsCapturing() const;
  // Starts a capture, or stops the running one and writes a report.
  bool ToggleDiagnosticsCapture(std::filesystem::path* written_report = nullptr);
  // Writes the rolling history buffer without disturbing a running capture.
  bool SaveDiagnosticsHistory(std::filesystem::path* written_report = nullptr);
  void SetDiagnosticsOverlay(bool enabled);
  bool IsDiagnosticsOverlayEnabled() const;

private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko

namespace ModernGekko = moderngekko;
