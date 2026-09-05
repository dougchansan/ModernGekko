#pragma once

// Data model for .mgdiag reports plus the deterministic analyzer and the
// report comparison used by moderngekko-diag. Nothing here talks to the
// network or to a live runtime; it only reads and writes report documents.

#include "moderngekko/diagnostics.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace moderngekko::diagnostics
{
struct BuildInfo
{
  std::string version;
  std::string git_commit;
  // "clean", "dirty" or "unknown".
  std::string git_state = "unknown";
  std::string configuration;
  std::string compiler;
  std::string compiler_version;
  std::string host_architecture;
  std::string cpu_backend;
  bool link_time_optimization = false;
  std::string build_timestamp;
};

BuildInfo CurrentBuildInfo();

struct SystemInfo
{
  std::string cpu_model;
  int physical_cores = 0;
  int logical_processors = 0;
  std::string architecture;
  std::vector<std::string> instruction_sets;
  std::uint64_t total_physical_memory_bytes = 0;
  std::uint64_t process_resident_bytes = 0;
  std::uint64_t process_peak_bytes = 0;
  std::string os_name;
  std::string os_version;
  std::string os_architecture;
};

SystemInfo CollectSystemInfo();

struct FrameStatistics
{
  std::size_t frame_count = 0;
  double duration_s = 0.0;
  double average_fps = 0.0;
  double average_frame_ms = 0.0;
  double median_frame_ms = 0.0;
  double p90_frame_ms = 0.0;
  double p95_frame_ms = 0.0;
  double p99_frame_ms = 0.0;
  // Only meaningful with at least 1000 frames; -1 when unavailable.
  double p999_frame_ms = -1.0;
  double max_frame_ms = 0.0;
  double low_1_percent_fps = 0.0;
  // -1 when the sample count is too small to be meaningful.
  double low_01_percent_fps = -1.0;
  double over_budget_percent = 0.0;
  double target_frame_ms = 0.0;
  double average_speed = 0.0;
  double min_speed = 0.0;
  double average_vps = 0.0;
};

FrameStatistics ComputeFrameStatistics(const std::vector<FrameRecord>& frames,
                                       double target_frame_ms);

enum class Classification
{
  Unknown,
  GuestCpuBound,
  StaticRecompSlowPathBound,
  GpuBound,
  CpuGpuSynchronizationBound,
  RendererCpuBound,
  ShaderCompilationHitching,
  TextureOrEfbTransferBound,
  AudioOrTimingWait,
  NetplayWait,
  ThreadContention,
  PossibleHardwareThrottling,
  InsufficientEvidence,
};

const char* ClassificationName(Classification value);

enum class Confidence
{
  Low,
  Medium,
  High,
};

const char* ConfidenceName(Confidence value);

struct Finding
{
  Classification classification = Classification::Unknown;
  Confidence confidence = Confidence::Low;
  std::vector<std::string> evidence;
};

struct AnalysisInput
{
  FrameStatistics frames;
  // Mean milliseconds per frame spent in each zone.
  std::array<double, kZoneCount> zone_ms_per_frame{};
  // Total counter values over the capture.
  std::array<std::uint64_t, kCounterCount> counters{};
  std::vector<Event> events;
  std::vector<ThreadRecord> threads;
  Level level = Level::Basic;
};

struct Analysis
{
  std::vector<Finding> findings;
  std::string headline;
};

Analysis Analyze(const AnalysisInput& input);

// A parsed .mgdiag report, or one assembled in memory before writing.
struct Report
{
  int schema_version = kSchemaVersion;
  std::string created_utc;
  std::string capture_kind = "capture";
  Level level = Level::Basic;
  bool anonymized = true;

  BuildInfo build;
  SystemInfo system;
  GameIdentity game;
  ModuleIdentity module;
  GraphicsIdentity graphics;
  std::map<std::string, std::string> runtime_config;

  FrameStatistics statistics;
  std::array<double, kZoneCount> zone_ms_per_frame{};
  std::array<std::uint64_t, kCounterCount> counters{};

  std::vector<FrameRecord> frames;
  std::vector<Event> events;
  std::vector<ThreadRecord> threads;
  std::vector<HotspotRecord> hotspots;
  std::vector<std::string> log_lines;

  Analysis analysis;
};

struct WriteResult
{
  bool ok = false;
  std::string error;
};

// Writes a single-file, ZIP-compatible .mgdiag archive.
WriteResult WriteReport(const Report& report, const std::filesystem::path& path);

struct ReadResult
{
  bool ok = false;
  std::string error;
  Report report;
};

ReadResult ReadReport(const std::filesystem::path& path);

// Human-readable renderings used by moderngekko-diag.
std::string RenderInfo(const Report& report);
std::string RenderSummary(const Report& report);

struct ComparisonRow
{
  std::string label;
  double a = 0.0;
  double b = 0.0;
  std::string unit;
  // true when a larger value is worse (frame times, waits).
  bool higher_is_worse = true;
  // true for per-frame subsystem timings, which are the only rows eligible to
  // be named as the dominant difference.
  bool is_zone = false;
};

struct Comparison
{
  std::vector<std::string> mismatches;
  std::vector<ComparisonRow> rows;
  std::string dominant_difference;
};

Comparison CompareReports(const Report& a, const Report& b);
std::string RenderComparison(const Report& a, const Report& b, const Comparison& comparison);
}  // namespace moderngekko::diagnostics
