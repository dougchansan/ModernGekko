#pragma once

// ModernGekko diagnostics: low-overhead telemetry collection for performance
// reports. The subsystem observes the runtime; it never drives emulation.
//
// Hot-path entry points (MG_PERF_SCOPE, Count, NoteGuestPc) compile down to a
// relaxed load of one global plus a predictable branch when diagnostics are
// disabled, so instrumentation can sit in production builds.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moderngekko::diagnostics
{
// Bumped whenever the on-disk report layout changes incompatibly.
inline constexpr int kSchemaVersion = 1;

enum class Level : std::uint32_t
{
  Off = 0,
  Basic = 1,
  Detailed = 2,
  Trace = 3,
};

const char* LevelName(Level level);
bool ParseLevel(std::string_view text, Level* out);

// Scoped timing zones. The order is part of the report layout: append only.
enum class Zone : std::uint8_t
{
  GuestCpu,
  StaticRecompDispatch,
  InterpreterFallback,
  GxCommandProcessor,
  VertexLoader,
  TextureDecoder,
  ShaderGeneration,
  PipelineCreation,
  RendererSubmission,
  GpuWait,
  GpuExecution,
  Present,
  Dsp,
  Audio,
  Memory,
  Mmio,
  Scheduler,
  Synchronization,
  Mods,
  Netplay,
  Other,
};
inline constexpr std::size_t kZoneCount = static_cast<std::size_t>(Zone::Other) + 1;
const char* ZoneName(Zone zone);

// Free-running counters. The order is part of the report layout: append only.
enum class Counter : std::uint8_t
{
  StaticRecompDispatches,
  StaticRecompDispatchMisses,
  InterpreterFallbacks,
  UnsupportedInstructionFallbacks,
  GuestInstructions,
  IndirectBranches,
  IndirectBranchFastHits,
  IndirectBranchSlowLookups,
  MmioReads,
  MmioWrites,
  MmioSlowPaths,
  Exceptions,
  Interrupts,
  CodeInvalidations,
  HostCalls,
  ModHostCalls,
  GxCommands,
  GxUnknownOpcodes,
  GxDisplayLists,
  DrawCalls,
  PrimitivesLoaded,
  VerticesLoaded,
  TextureDecodes,
  TextureDecodeBytes,
  TextureUploads,
  TextureUploadBytes,
  BufferUploads,
  EfbReads,
  EfbWrites,
  EfbCopies,
  FramebufferReadbacks,
  PipelineSwitches,
  PipelineCreations,
  ShaderCacheHits,
  ShaderCacheMisses,
  ShaderCompilations,
  QueueSubmissions,
  SynchronizationEvents,
  GpuFenceWaits,
  SchedulerSleeps,
  NetplayInputWaits,
  AudioUnderruns,
};
inline constexpr std::size_t kCounterCount =
    static_cast<std::size_t>(Counter::AudioUnderruns) + 1;
const char* CounterName(Counter counter);

enum class EventType : std::uint8_t
{
  CaptureStart,
  CaptureStop,
  ShaderCompilation,
  PipelineCompilation,
  ShaderCacheMiss,
  GpuStall,
  LongFrame,
  Note,
};
const char* EventTypeName(EventType type);

struct Event
{
  double time_s = 0.0;
  EventType type = EventType::Note;
  double duration_ms = 0.0;
  std::uint64_t hash = 0;
  std::string detail;
};

// One frame of telemetry. Zone times and counters are per-frame deltas.
struct FrameRecord
{
  double time_s = 0.0;
  double frame_ms = 0.0;
  double fps = 0.0;
  double vps = 0.0;
  double speed = 0.0;
  std::array<float, kZoneCount> zone_ms{};
  std::array<std::uint32_t, kCounterCount> counters{};
};

// What the runtime knows about a frame that diagnostics cannot measure itself.
struct FrameTelemetry
{
  double fps = 0.0;
  double vps = 0.0;
  // Emulation speed as a fraction of full speed (1.0 == 100%).
  double speed = 0.0;
};

struct ThreadRecord
{
  std::string name;
  std::uint64_t id = 0;
  // Seconds of CPU time consumed during the capture, or -1 when the platform
  // cannot report it.
  double cpu_seconds = -1.0;
  double utilization = -1.0;
  std::array<double, kZoneCount> zone_ms{};
};

struct HotspotRecord
{
  std::uint32_t guest_pc = 0;
  std::string symbol;
  std::uint64_t samples = 0;
  double percent = 0.0;
};

struct GameIdentity
{
  std::string title;
  std::string disc_id;
  std::string platform;
  std::string dol_sha256;
  std::string rel_sha256;
  std::string assets_sha256;
  std::uint32_t entry_point = 0;
};

struct ModuleIdentity
{
  // "dynamic", "attached" or "none".
  std::string kind = "none";
  std::string file_name;
  std::string sha256;
};

struct GraphicsIdentity
{
  std::string backend;
  std::string adapter;
  std::string api_version;
  std::string driver;
  int internal_resolution_scale = 0;
  std::string shader_compilation_mode;
  bool vsync = false;
};

struct Config
{
  bool enabled = false;
  Level level = Level::Basic;
  bool overlay = false;
  bool anonymize = true;
  // 0 means "capture until the user stops it".
  double capture_seconds = 0.0;
  double history_seconds = 30.0;
  // Guest PC sampling rate; clamped to [50, 2000] Hz.
  unsigned sample_hz = 500;
  // Frames slower than this are counted as over budget.
  double target_frame_ms = 16.7;
  std::filesystem::path output_directory;
};

struct CaptureResult
{
  bool ok = false;
  std::filesystem::path report_path;
  std::string error;
  std::size_t frame_count = 0;
  double duration_s = 0.0;
};

namespace detail
{
// 0 == Level::Off. Read on every instrumented hot path.
extern std::atomic<std::uint32_t> g_level;

// Per-thread accumulators. Only the owning thread writes, so the relaxed
// load/store pairs below compile to plain moves while remaining race-free for
// the aggregating reader.
struct ThreadState
{
  std::array<std::atomic<std::uint64_t>, kZoneCount> zone_ns{};
  std::array<std::atomic<std::uint64_t>, kZoneCount> zone_hits{};
  std::array<std::atomic<std::uint64_t>, kCounterCount> counters{};
  std::atomic<std::uint32_t> guest_pc{0};
  std::atomic<std::uint64_t> guest_pc_epoch{0};
  // -1 until the owning thread samples its own CPU time; reading another
  // thread's CPU time is not portable, so threads publish their own.
  std::atomic<double> cpu_seconds{-1.0};
  std::uint64_t os_thread_id = 0;
};

extern thread_local ThreadState* t_state;
ThreadState& AcquireThreadState();

inline ThreadState& Tls()
{
  return t_state != nullptr ? *t_state : AcquireThreadState();
}

inline void AddZone(ThreadState& state, std::size_t zone, std::uint64_t nanoseconds)
{
  auto& slot = state.zone_ns[zone];
  slot.store(slot.load(std::memory_order_relaxed) + nanoseconds, std::memory_order_relaxed);
  auto& hits = state.zone_hits[zone];
  hits.store(hits.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

inline void AddCounter(ThreadState& state, std::size_t counter, std::uint64_t amount)
{
  auto& slot = state.counters[counter];
  slot.store(slot.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

inline void PublishGuestPc(ThreadState& state, std::uint32_t pc)
{
  state.guest_pc.store(pc, std::memory_order_relaxed);
  state.guest_pc_epoch.store(state.guest_pc_epoch.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
}
}  // namespace detail

inline bool Active(Level minimum = Level::Basic)
{
  return detail::g_level.load(std::memory_order_relaxed) >=
         static_cast<std::uint32_t>(minimum);
}

inline void Count(Counter counter, std::uint64_t amount = 1)
{
  if (!Active())
    return;
  detail::AddCounter(detail::Tls(), static_cast<std::size_t>(counter), amount);
}

// Adds externally measured time to a zone (GPU timestamp queries, for example).
inline void AddZoneNanos(Zone zone, std::uint64_t nanoseconds)
{
  if (!Active())
    return;
  detail::AddZone(detail::Tls(), static_cast<std::size_t>(zone), nanoseconds);
}

// Publishes the guest program counter for the periodic sampler. One relaxed
// store; no timer is read on the emulation thread.
inline void NoteGuestPc(std::uint32_t pc)
{
  if (!Active(Level::Trace))
    return;
  detail::PublishGuestPc(detail::Tls(), pc);
}

class ScopeTimer final
{
public:
  explicit ScopeTimer(Zone zone, Level minimum = Level::Detailed)
      : m_zone(static_cast<std::size_t>(zone))
  {
    if (Active(minimum))
      m_start = std::chrono::steady_clock::now();
  }

  ~ScopeTimer()
  {
    if (m_start.time_since_epoch().count() == 0)
      return;
    const auto elapsed = std::chrono::steady_clock::now() - m_start;
    detail::AddZone(detail::Tls(), m_zone,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  ScopeTimer(const ScopeTimer&) = delete;
  ScopeTimer& operator=(const ScopeTimer&) = delete;

private:
  std::size_t m_zone;
  std::chrono::steady_clock::time_point m_start{};
};

#define MG_PERF_CONCAT_INNER(a, b) a##b
#define MG_PERF_CONCAT(a, b) MG_PERF_CONCAT_INNER(a, b)
// Scoped zone timing. Active from Level::Detailed upwards: reading a clock
// twice costs tens of nanoseconds, so Basic captures stay counters-only.
#define MG_PERF_SCOPE(zone)                                                              \
  ::moderngekko::diagnostics::ScopeTimer MG_PERF_CONCAT(mg_perf_scope_, __LINE__)(zone)
// For zones on paths hot enough that even a detailed capture should not pay
// for a clock read; pass Level::Trace there.
#define MG_PERF_SCOPE_AT(zone, level)                                                    \
  ::moderngekko::diagnostics::ScopeTimer MG_PERF_CONCAT(mg_perf_scope_, __LINE__)(zone, level)

// Process-wide diagnostics state. Every method is safe to call when
// diagnostics are disabled; the disabled paths do nothing.
class Diagnostics final
{
public:
  static Diagnostics& Get();

  void Initialize(Config config);
  void Shutdown();

  bool IsEnabled() const;
  bool IsCapturing() const;
  Level GetLevel() const;
  const Config& GetConfig() const;

  void SetGameIdentity(GameIdentity identity);
  void SetModuleIdentity(ModuleIdentity identity);
  void SetGraphicsIdentity(GraphicsIdentity identity);
  // Registers a guest address -> symbol table for hotspot reporting. Optional.
  void SetGuestSymbols(std::vector<std::pair<std::uint32_t, std::string>> symbols);
  bool LoadGuestSymbols(const std::filesystem::path& path, std::string* error = nullptr);

  // Names the calling thread for threads.csv.
  void NameCurrentThread(std::string name);
  // Publishes the calling thread's own CPU time. Cheap enough to call once
  // per frame; threads that never call it are reported as unavailable.
  void SampleCurrentThreadCpu();

  // Appends a line to the bounded log captured into the report.
  void AppendLog(std::string line);

  bool StartCapture();
  CaptureResult StopCapture();
  // Writes the rolling history buffer without disturbing an active capture.
  CaptureResult SaveHistory();
  void ToggleCapture();

  // Called once per presented frame from the thread that presents.
  void EndFrame(const FrameTelemetry& telemetry);

  void RecordEvent(EventType type, double duration_ms, std::uint64_t hash = 0,
                   std::string detail = {});

  // Snapshot of the most recent frames, oldest first. For the overlay.
  std::vector<FrameRecord> RecentFrames(std::size_t count) const;

  Diagnostics(const Diagnostics&) = delete;
  Diagnostics& operator=(const Diagnostics&) = delete;

private:
  Diagnostics();
  ~Diagnostics();

  CaptureResult WriteCapture(const std::vector<FrameRecord>& frames,
                             const std::vector<Event>& events, std::string capture_kind);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Replaces user-identifying path components ("/home/doug/Games" ->
// "<USER>/Games"). Returns generic separators so reports compare across hosts.
std::string AnonymizePath(const std::filesystem::path& path);
std::string AnonymizeText(std::string text);
}  // namespace moderngekko::diagnostics
