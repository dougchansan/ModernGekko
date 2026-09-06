#include "diagnostics_internal.hpp"
#include "moderngekko/diagnostics.hpp"
#include "moderngekko/diagnostics_report.hpp"

#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace moderngekko::diagnostics;

namespace
{
int g_failures = 0;

void Check(bool condition, const char* what)
{
  if (condition)
    return;
  std::fprintf(stderr, "FAILED: %s\n", what);
  ++g_failures;
}

void CheckNear(double actual, double expected, double tolerance, const char* what)
{
  if (std::fabs(actual - expected) <= tolerance)
    return;
  std::fprintf(stderr, "FAILED: %s (got %f, expected %f)\n", what, actual, expected);
  ++g_failures;
}

void SetEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  ::setenv(name, value, 1);
#endif
}

std::vector<FrameRecord> SyntheticFrames(const std::vector<double>& frame_times_ms, double speed)
{
  std::vector<FrameRecord> frames;
  double time_s = 0.0;
  frames.reserve(frame_times_ms.size());
  for (const double ms : frame_times_ms)
  {
    FrameRecord frame;
    time_s += ms / 1000.0;
    frame.time_s = time_s;
    frame.frame_ms = ms;
    frame.fps = ms > 0.0 ? 1000.0 / ms : 0.0;
    frame.vps = frame.fps;
    frame.speed = speed;
    frames.push_back(frame);
  }
  return frames;
}

void TestRingBufferBounds()
{
  detail::RingBuffer<int> ring;
  ring.Reset(8);
  Check(ring.Capacity() == 8, "ring buffer honours its capacity");
  Check(ring.Size() == 0, "ring buffer starts empty");
  for (int i = 0; i < 100; ++i)
    ring.Push(i);
  Check(ring.Size() == 8, "ring buffer never exceeds its capacity");
  Check(ring.DroppedCount() == 92, "ring buffer counts overwritten entries");
  const std::vector<int> snapshot = ring.Snapshot();
  Check(snapshot.size() == 8, "ring buffer snapshot matches its size");
  Check(snapshot.front() == 92 && snapshot.back() == 99,
        "ring buffer snapshot keeps the newest entries oldest first");
  ring.Clear();
  Check(ring.Size() == 0 && ring.DroppedCount() == 0, "ring buffer clears");

  // A zero-capacity ring must swallow pushes rather than allocate.
  detail::RingBuffer<int> empty;
  empty.Reset(0);
  empty.Push(1);
  Check(empty.Size() == 0 && empty.DroppedCount() == 1, "zero-capacity ring drops pushes");
}

void TestStatistics()
{
  Check(ComputeFrameStatistics({}, 16.7).frame_count == 0, "empty capture yields empty statistics");

  std::vector<double> times;
  for (int i = 1; i <= 100; ++i)
    times.push_back(static_cast<double>(i));
  const FrameStatistics stats = ComputeFrameStatistics(SyntheticFrames(times, 1.0), 16.7);
  Check(stats.frame_count == 100, "statistics count every frame");
  CheckNear(stats.average_frame_ms, 50.5, 1e-9, "average frame time");
  CheckNear(stats.median_frame_ms, 50.0, 1e-9, "median frame time");
  CheckNear(stats.p90_frame_ms, 90.0, 1e-9, "p90 frame time");
  CheckNear(stats.p95_frame_ms, 95.0, 1e-9, "p95 frame time");
  CheckNear(stats.p99_frame_ms, 99.0, 1e-9, "p99 frame time");
  CheckNear(stats.max_frame_ms, 100.0, 1e-9, "maximum frame time");
  Check(stats.p999_frame_ms < 0.0, "p99.9 is withheld below 1000 samples");
  Check(stats.low_01_percent_fps < 0.0, "0.1% low is withheld below 1000 samples");
  CheckNear(stats.low_1_percent_fps, 10.0, 1e-9, "1% low uses the slowest 1% of frames");
  CheckNear(stats.over_budget_percent, 84.0, 1e-9, "frames over a 16.7 ms budget");
  CheckNear(stats.average_speed, 1.0, 1e-9, "average speed");

  std::vector<double> many;
  for (int i = 1; i <= 2000; ++i)
    many.push_back(static_cast<double>(i) / 100.0);
  const FrameStatistics large = ComputeFrameStatistics(SyntheticFrames(many, 0.5), 16.7);
  Check(large.p999_frame_ms > 0.0, "p99.9 is reported at 1000 or more samples");
  Check(large.low_01_percent_fps > 0.0, "0.1% low is reported at 1000 or more samples");
  CheckNear(large.p999_frame_ms, 19.98, 1e-9, "p99.9 uses nearest-rank ordering");
  CheckNear(large.min_speed, 0.5, 1e-9, "minimum speed is tracked");

  // A single frame must not divide by zero or produce a negative percentile.
  const FrameStatistics single = ComputeFrameStatistics(SyntheticFrames({20.0}, 1.0), 16.7);
  Check(single.frame_count == 1, "single-frame capture is still summarized");
  CheckNear(single.p99_frame_ms, 20.0, 1e-9, "single-frame percentile is the sample itself");
  CheckNear(single.low_1_percent_fps, 50.0, 1e-9, "single-frame 1% low");
}

void TestAnonymization()
{
  SetEnvironment("HOME", "/home/diagtestuser");
  SetEnvironment("USERPROFILE", "/home/diagtestuser");
  SetEnvironment("USER", "diagtestuser");
  SetEnvironment("USERNAME", "diagtestuser");
  SetEnvironment("LOGNAME", "diagtestuser");

  const std::string path = AnonymizePath(fs::path("/home/diagtestuser/Games/One Piece"));
  Check(path == "<USER>/Games/One Piece", "home directory prefixes are replaced");
  Check(AnonymizeText("loaded /home/diagtestuser/Mods/x.mgm").find("diagtestuser") ==
            std::string::npos,
        "user names are stripped from free text");
  Check(AnonymizeText("peer 192.168.1.44 joined") == "peer <IP> joined",
        "IPv4 addresses are redacted");
  Check(AnonymizeText("build 1.2.3.4 ok") == "build <IP> ok",
        "dotted quads inside text are redacted conservatively");
  Check(AnonymizeText("scale 4 at 60 fps") == "scale 4 at 60 fps",
        "ordinary numbers survive redaction");
}

void TestArchiveRoundTrip()
{
  const fs::path path = fs::temp_directory_path() / "moderngekko-diag-archive-test.mgdiag";
  fs::remove(path);
  const std::vector<detail::ArchiveEntry> entries = {
      {"report.json", "{\"schema_version\":1}"},
      {"frames.csv", "time_s,frame_ms\n0.0,16.7\n"},
      {"empty.txt", ""},
  };
  std::string error;
  Check(detail::WriteArchive(path, entries, &error), "archive writes");

  std::vector<detail::ArchiveEntry> read;
  Check(detail::ReadArchive(path, &read, &error), "archive reads back");
  Check(read.size() == entries.size(), "archive round-trips every entry");
  for (std::size_t i = 0; i < read.size() && i < entries.size(); ++i)
  {
    Check(read[i].name == entries[i].name && read[i].data == entries[i].data,
          "archive entry round-trips unchanged");
  }

  // Reports are shared over the internet; a corrupted one must be rejected.
  std::string bytes;
  {
    std::ifstream file(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  Check(bytes.size() > 64, "archive has content");
  bytes[60] = static_cast<char>(bytes[60] ^ 0xFF);
  const fs::path corrupt = fs::temp_directory_path() / "moderngekko-diag-corrupt-test.mgdiag";
  std::ofstream(corrupt, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  Check(!detail::ReadArchive(corrupt, &read, &error), "corrupted archives are rejected");
  fs::remove(path);
  fs::remove(corrupt);
}

void TestDisabledPath()
{
  Config config;
  config.enabled = false;
  config.output_directory = fs::temp_directory_path() / "moderngekko-diag-disabled";
  Diagnostics& diagnostics = Diagnostics::Get();
  diagnostics.Initialize(config);

  Check(!Active(), "instrumentation is inert while diagnostics are disabled");
  Check(!diagnostics.IsEnabled(), "disabled diagnostics report themselves disabled");
  Check(diagnostics.GetLevel() == Level::Off, "disabled diagnostics report level off");
  Check(!diagnostics.StartCapture(), "capture cannot start while disabled");

  // None of these may crash, allocate a report, or touch the filesystem.
  Count(Counter::DrawCalls, 10);
  AddZoneNanos(Zone::GpuWait, 1000);
  NoteGuestPc(0x80003100u);
  diagnostics.RecordEvent(EventType::Note, 1.0, 0, "ignored");
  diagnostics.EndFrame(FrameTelemetry{60.0, 60.0, 1.0});
  Check(diagnostics.RecentFrames(16).empty(), "no frames are recorded while disabled");
  Check(!fs::exists(config.output_directory), "no report directory is created while disabled");

  const CaptureResult result = diagnostics.SaveHistory();
  Check(!result.ok, "saving history while disabled fails cleanly");
  diagnostics.Shutdown();
}

void TestCaptureLifecycle()
{
  const fs::path output = fs::temp_directory_path() / "moderngekko-diag-capture";
  fs::remove_all(output);

  Config config;
  config.enabled = true;
  config.level = Level::Detailed;
  config.output_directory = output;
  config.history_seconds = 5.0;
  config.anonymize = true;

  Diagnostics& diagnostics = Diagnostics::Get();
  diagnostics.Initialize(config);
  diagnostics.NameCurrentThread("Test");
  GameIdentity game;
  game.title = "Synthetic Title";
  game.disc_id = "GTEST0";
  game.platform = "GameCube";
  game.dol_sha256 = std::string(64, 'a');
  diagnostics.SetGameIdentity(game);

  Check(diagnostics.IsEnabled(), "enabled diagnostics report themselves enabled");
  Check(Active(Level::Detailed), "detailed level activates detailed instrumentation");
  Check(!Active(Level::Trace), "detailed level does not activate trace instrumentation");

  // Stopping without starting must not produce a report.
  Check(!diagnostics.StopCapture().ok, "stopping an idle capture fails cleanly");

  Check(diagnostics.StartCapture(), "capture starts");
  Check(diagnostics.IsCapturing(), "capture reports itself running");
  Check(!diagnostics.StartCapture(), "a second capture cannot start while one runs");

  for (int i = 0; i < 30; ++i)
  {
    Count(Counter::DrawCalls, 4);
    Count(Counter::InterpreterFallbacks);
    AddZoneNanos(Zone::GuestCpu, 2000000);
    AddZoneNanos(Zone::GpuWait, 5000000);
    diagnostics.RecordEvent(EventType::ShaderCompilation, 12.5, 0xABCD, "test shader");
    diagnostics.EndFrame(FrameTelemetry{60.0, 60.0, 1.0});
  }

  Check(diagnostics.RecentFrames(8).size() == 8, "the overlay can read recent frames");
  const CaptureResult result = diagnostics.StopCapture();
  Check(result.ok, "capture stops and writes a report");
  Check(!diagnostics.IsCapturing(), "capture reports itself stopped");
  Check(result.frame_count == 30, "every frame reaches the report");
  Check(fs::exists(result.report_path), "the report file exists");
  Check(result.report_path.extension() == ".mgdiag", "the report uses the .mgdiag extension");

  const ReadResult read = ReadReport(result.report_path);
  Check(read.ok, "the written report parses");
  if (read.ok)
  {
    Check(read.report.schema_version == kSchemaVersion, "the report carries the schema version");
    Check(read.report.game.disc_id == "GTEST0", "game identity survives the round trip");
    Check(read.report.game.dol_sha256 == std::string(64, 'a'), "the DOL hash is preserved");
    Check(read.report.counters[static_cast<std::size_t>(Counter::DrawCalls)] == 120,
          "counter totals match what was recorded");
    Check(read.report.counters[static_cast<std::size_t>(Counter::InterpreterFallbacks)] == 30,
          "counters accumulate once per call");
    Check(read.report.zone_ms_per_frame[static_cast<std::size_t>(Zone::GuestCpu)] > 1.5,
          "zone times are attributed per frame");
    Check(read.report.frames.size() == 30, "frames.csv round-trips");
    Check(!read.report.events.empty(), "events reach the report");
    Check(!read.report.build.git_commit.empty(), "build identity is present");
    Check(!read.report.system.cpu_model.empty(), "system identity is present");
  }

  // Counters must be per-capture: a second capture starts from zero.
  Check(diagnostics.StartCapture(), "a second capture can start after the first stops");
  for (int i = 0; i < 12; ++i)
  {
    Count(Counter::DrawCalls, 1);
    diagnostics.EndFrame(FrameTelemetry{30.0, 60.0, 0.5});
  }
  const CaptureResult second = diagnostics.StopCapture();
  Check(second.ok, "the second capture writes a report");
  const ReadResult second_read = ReadReport(second.report_path);
  Check(second_read.ok, "the second report parses");
  if (second_read.ok)
  {
    Check(second_read.report.counters[static_cast<std::size_t>(Counter::DrawCalls)] == 12,
          "counters reset between captures");
  }

  // An immediately stopped capture is legal and must still produce a report.
  Check(diagnostics.StartCapture(), "an empty capture starts");
  const CaptureResult empty = diagnostics.StopCapture();
  Check(empty.ok, "an empty capture still writes a report");
  Check(empty.frame_count == 0, "an empty capture reports no frames");
  const ReadResult empty_read = ReadReport(empty.report_path);
  Check(empty_read.ok, "an empty report parses");
  if (empty_read.ok)
  {
    Check(empty_read.report.analysis.findings.size() == 1 &&
              empty_read.report.analysis.findings.front().classification ==
                  Classification::InsufficientEvidence,
          "an empty capture is classified as insufficient evidence");
  }

  const CaptureResult history = diagnostics.SaveHistory();
  Check(history.ok, "the rolling history can be saved");
  Check(history.report_path.string().find("history") != std::string::npos,
        "history reports are named as history captures");

  diagnostics.Shutdown();
  Check(!Active(), "shutdown disables instrumentation");
  fs::remove_all(output);
}

// Scope timers are gated one level above counters, so a basic capture never
// pays for a clock read on an instrumented path.
void TestScopeGating()
{
  const fs::path output = fs::temp_directory_path() / "moderngekko-diag-gating";
  fs::remove_all(output);
  Config config;
  config.enabled = true;
  config.output_directory = output;

  Diagnostics& diagnostics = Diagnostics::Get();
  const auto zone_after_frame = [&](Level level) {
    config.level = level;
    diagnostics.Initialize(config);
    {
      MG_PERF_SCOPE(Zone::TextureDecoder);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Count(Counter::TextureDecodes);
    diagnostics.EndFrame(FrameTelemetry{60.0, 60.0, 1.0});
    const std::vector<FrameRecord> frames = diagnostics.RecentFrames(1);
    const FrameRecord frame = frames.empty() ? FrameRecord{} : frames.front();
    diagnostics.Shutdown();
    return frame;
  };

  const FrameRecord basic = zone_after_frame(Level::Basic);
  Check(basic.counters[static_cast<std::size_t>(Counter::TextureDecodes)] == 1,
        "basic captures still record counters");
  Check(basic.zone_ms[static_cast<std::size_t>(Zone::TextureDecoder)] == 0.0f,
        "basic captures do not pay for scope timers");

  const FrameRecord detailed = zone_after_frame(Level::Detailed);
  Check(detailed.counters[static_cast<std::size_t>(Counter::TextureDecodes)] == 1,
        "detailed captures record counters");
  Check(detailed.zone_ms[static_cast<std::size_t>(Zone::TextureDecoder)] >= 1.0f,
        "detailed captures record scope timings");
  fs::remove_all(output);
}

void TestThreadedAggregation()
{
  const fs::path output = fs::temp_directory_path() / "moderngekko-diag-threads";
  fs::remove_all(output);

  Config config;
  config.enabled = true;
  config.level = Level::Detailed;
  config.output_directory = output;

  Diagnostics& diagnostics = Diagnostics::Get();
  diagnostics.Initialize(config);
  Check(diagnostics.StartCapture(), "threaded capture starts");

  constexpr int kThreads = 4;
  constexpr int kIterations = 5000;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i)
  {
    workers.emplace_back([] {
      Diagnostics::Get().NameCurrentThread("Worker");
      for (int j = 0; j < kIterations; ++j)
      {
        Count(Counter::GxCommands);
        AddZoneNanos(Zone::GxCommandProcessor, 100);
      }
      Diagnostics::Get().SampleCurrentThreadCpu();
    });
  }
  for (std::thread& worker : workers)
    worker.join();
  diagnostics.EndFrame(FrameTelemetry{60.0, 60.0, 1.0});

  const CaptureResult result = diagnostics.StopCapture();
  Check(result.ok, "threaded capture writes a report");
  const ReadResult read = ReadReport(result.report_path);
  Check(read.ok, "threaded report parses");
  if (read.ok)
  {
    Check(read.report.counters[static_cast<std::size_t>(Counter::GxCommands)] ==
              kThreads * kIterations,
          "counters from every thread are aggregated exactly once");
    Check(read.report.threads.size() >= 2, "per-thread rows are emitted");
  }
  diagnostics.Shutdown();
  fs::remove_all(output);
}
}  // namespace

int main()
{
  TestRingBufferBounds();
  TestStatistics();
  TestAnonymization();
  TestArchiveRoundTrip();
  TestDisabledPath();
  TestCaptureLifecycle();
  TestScopeGating();
  TestThreadedAggregation();
  if (g_failures != 0)
    std::fprintf(stderr, "%d diagnostics check(s) failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
