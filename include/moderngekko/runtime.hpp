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
};

struct AudioSettings
{
  std::string backend;
};

struct InputSettings
{
  bool background_input = false;
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
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  bool fullscreen = false;
  bool allow_interpreter = false;
  bool show_fps_in_title = true;
  std::optional<std::string> window_title;

  // Benchmark automation. All of these hang off Dolphin's existing ~1 Hz
  // Host_UpdateTitle tick, so they add no thread of their own and inherit its
  // guarantee of running on the host thread with the core live.
  //
  // A benchmark needs a fixed scene, an uncapped run, and a machine-readable
  // speed; without these the only signal is a present-rate counter in the
  // window title, which measures nothing once the emulator is uncapped.
  std::filesystem::path status_file;
  std::filesystem::path load_state_path;
  std::filesystem::path save_state_path;
  double load_state_after_seconds = 5.0;
  double save_state_after_seconds = 0.0;
  double run_seconds = 0.0;
  // Buttons held on port 0 once the run starts, as a GCPadStatus mask.
  // Benchmarking a parked savestate understates load; holding accelerate
  // keeps the kart driving so track geometry keeps streaming in.
  unsigned int hold_buttons = 0;
  // Buttons pressed and released repeatedly on port 0, as a GCPadStatus mask.
  // A menu advances on the press *edge*, so a permanent hold advances at most
  // one screen; walking a fresh boot through to a race needs a release between
  // presses.
  unsigned int spam_buttons = 0;
  // Seconds each half of the press/release cycle lasts. Slow enough that a
  // menu's own debounce and transition animations do not swallow the edge.
  double spam_period_seconds = 0.25;
  // Stop spamming this many seconds in, releasing the buttons. START pauses a
  // race and A dismisses the pause, so spam that outlives the menus cycles the
  // pause screen forever and the race never runs. 0 means never stop.
  double spam_stop_seconds = 0.0;
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
