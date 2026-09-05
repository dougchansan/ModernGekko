#include "diagnostics_internal.hpp"
#include "moderngekko/diagnostics_report.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
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

AnalysisInput MakeInput(double frame_ms, double speed, std::size_t frames = 600)
{
  AnalysisInput input;
  input.level = Level::Detailed;
  input.frames.frame_count = frames;
  input.frames.duration_s = static_cast<double>(frames) * frame_ms / 1000.0;
  input.frames.average_frame_ms = frame_ms;
  input.frames.median_frame_ms = frame_ms;
  input.frames.p99_frame_ms = frame_ms * 1.2;
  input.frames.max_frame_ms = frame_ms * 1.5;
  input.frames.average_fps = 1000.0 / frame_ms;
  input.frames.average_speed = speed;
  input.frames.min_speed = speed;
  input.frames.target_frame_ms = 16.7;
  input.frames.over_budget_percent = frame_ms > 16.7 ? 100.0 : 0.0;
  return input;
}

void SetZone(AnalysisInput& input, Zone zone, double ms)
{
  input.zone_ms_per_frame[static_cast<std::size_t>(zone)] = ms;
}

void SetCounter(AnalysisInput& input, Counter counter, std::uint64_t value)
{
  input.counters[static_cast<std::size_t>(counter)] = value;
}

Classification TopClassification(const Analysis& analysis)
{
  return analysis.findings.empty() ? Classification::Unknown
                                   : analysis.findings.front().classification;
}

void TestSynchronizationVerdict()
{
  AnalysisInput input = MakeInput(24.0, 0.71);
  SetZone(input, Zone::GuestCpu, 7.8);
  SetZone(input, Zone::GpuExecution, 5.1);
  SetZone(input, Zone::GpuWait, 8.9);
  SetZone(input, Zone::RendererSubmission, 2.1);
  SetCounter(input, Counter::InterpreterFallbacks, 0);

  const Analysis analysis = Analyze(input);
  Check(TopClassification(analysis) == Classification::CpuGpuSynchronizationBound,
        "a dominant GPU wait is classified as a synchronization bottleneck");
  Check(analysis.findings.front().confidence == Confidence::High,
        "a 37% GPU wait is a high-confidence verdict");
  Check(analysis.findings.front().evidence.size() >= 5,
        "the synchronization verdict cites its evidence");

  bool mentions_wait = false;
  bool mentions_pacing = false;
  for (const std::string& line : analysis.findings.front().evidence)
  {
    mentions_wait = mentions_wait || line.find("GPU wait") != std::string::npos;
    mentions_pacing = mentions_pacing || line.find("intended pacing") != std::string::npos;
  }
  Check(mentions_wait, "the evidence names the GPU wait");
  Check(mentions_pacing, "a below-full-speed capture is called out as such");

  // The analyzer is a pure function of its input.
  const Analysis repeat = Analyze(input);
  Check(repeat.headline == analysis.headline &&
            repeat.findings.size() == analysis.findings.size(),
        "the analyzer is deterministic");
}

void TestGuestCpuVerdict()
{
  AnalysisInput input = MakeInput(30.0, 0.55);
  SetZone(input, Zone::GuestCpu, 12.0);
  SetZone(input, Zone::StaticRecompDispatch, 8.0);
  SetZone(input, Zone::RendererSubmission, 2.0);
  SetZone(input, Zone::GpuExecution, 3.0);
  SetCounter(input, Counter::StaticRecompDispatches, 900000);
  Check(TopClassification(Analyze(input)) == Classification::GuestCpuBound,
        "guest execution dominating the frame is a CPU-bound verdict");
}

void TestRendererVerdict()
{
  AnalysisInput input = MakeInput(20.0, 0.8);
  SetZone(input, Zone::RendererSubmission, 6.0);
  SetZone(input, Zone::GxCommandProcessor, 4.0);
  SetZone(input, Zone::VertexLoader, 2.0);
  SetZone(input, Zone::GuestCpu, 3.0);
  SetCounter(input, Counter::DrawCalls, 600000);
  Check(TopClassification(Analyze(input)) == Classification::RendererCpuBound,
        "renderer submission dominating the frame is a renderer-bound verdict");
}

void TestGpuVerdict()
{
  AnalysisInput input = MakeInput(20.0, 0.85);
  SetZone(input, Zone::GpuExecution, 13.0);
  SetZone(input, Zone::GuestCpu, 2.0);
  SetZone(input, Zone::RendererSubmission, 1.5);
  const Analysis analysis = Analyze(input);
  Check(TopClassification(analysis) == Classification::GpuBound,
        "measured GPU execution dominating the frame is a GPU-bound verdict");
  Check(analysis.findings.front().confidence == Confidence::High,
        "65% GPU execution is a high-confidence verdict");
}

void TestStaticRecompVerdict()
{
  AnalysisInput input = MakeInput(28.0, 0.6);
  SetZone(input, Zone::InterpreterFallback, 9.0);
  SetZone(input, Zone::GuestCpu, 1.0);
  SetCounter(input, Counter::StaticRecompDispatches, 100000);
  SetCounter(input, Counter::StaticRecompDispatchMisses, 20000);
  SetCounter(input, Counter::InterpreterFallbacks, 20000);
  SetCounter(input, Counter::UnsupportedInstructionFallbacks, 4);
  const Analysis analysis = Analyze(input);
  bool found = false;
  for (const Finding& finding : analysis.findings)
    found = found || finding.classification == Classification::StaticRecompSlowPathBound;
  Check(found, "interpreter fallbacks raise a StaticRecomp slow-path finding");
}

void TestShaderHitchVerdict()
{
  AnalysisInput input = MakeInput(17.0, 0.99, 1200);
  input.frames.median_frame_ms = 16.7;
  input.frames.max_frame_ms = 61.2;
  SetZone(input, Zone::GuestCpu, 6.0);
  SetZone(input, Zone::RendererSubmission, 4.0);
  Event compile;
  compile.time_s = 4.5;
  compile.type = EventType::ShaderCompilation;
  compile.duration_ms = 43.7;
  compile.detail = "pixel shader";
  input.events.push_back(compile);
  const Analysis analysis = Analyze(input);
  Check(TopClassification(analysis) == Classification::ShaderCompilationHitching,
        "a compilation longer than two median frames is a hitching verdict");
  Check(analysis.findings.front().confidence == Confidence::High,
        "a compilation far longer than a frame is high confidence");
}

void TestFullSpeedAndShortCaptures()
{
  AnalysisInput full = MakeInput(33.3, 1.0, 600);
  full.frames.target_frame_ms = 33.4;
  full.frames.over_budget_percent = 0.0;
  const Analysis analysis = Analyze(full);
  Check(analysis.headline.find("No bottleneck") != std::string::npos,
        "30 FPS at full emulation speed is not reported as a bottleneck");

  AnalysisInput half = MakeInput(33.3, 0.5, 600);
  SetZone(half, Zone::GuestCpu, 22.0);
  const Analysis slow = Analyze(half);
  Check(slow.headline.find("No bottleneck") == std::string::npos,
        "30 FPS at half emulation speed is reported as a problem");

  AnalysisInput tiny = MakeInput(16.7, 1.0, 4);
  Check(TopClassification(Analyze(tiny)) == Classification::InsufficientEvidence,
        "a capture of a handful of frames yields no verdict");

  AnalysisInput basic = MakeInput(16.7, 0.9, 600);
  basic.level = Level::Basic;
  Check(TopClassification(Analyze(basic)) == Classification::InsufficientEvidence,
        "basic captures without zone data do not guess a subsystem");
}

Report MakeReport(const char* disc_id, const char* module_hash, double sync_ms, double guest_ms)
{
  Report report;
  report.created_utc = "2026-01-01T00:00:00Z";
  report.level = Level::Detailed;
  report.game.title = "Synthetic Title";
  report.game.disc_id = disc_id;
  report.game.platform = "GameCube";
  report.game.dol_sha256 = std::string(64, 'b');
  report.game.entry_point = 0x80003100u;
  report.module.kind = "dynamic";
  report.module.file_name = "gTEST01_recomp.so";
  report.module.sha256 = module_hash;
  report.build.version = "test";
  report.build.git_commit = "0123456789abcdef";
  report.build.git_state = "clean";
  report.build.configuration = "RelWithDebInfo";
  report.build.compiler = "gcc";
  report.graphics.backend = "Vulkan";
  report.graphics.internal_resolution_scale = 2;
  report.system.cpu_model = "Synthetic CPU";
  report.statistics.frame_count = 100;
  report.statistics.duration_s = 2.0;
  report.statistics.average_fps = 50.0;
  report.statistics.average_frame_ms = 20.0;
  report.statistics.p99_frame_ms = 24.0;
  report.statistics.p999_frame_ms = -1.0;
  report.statistics.low_1_percent_fps = 30.0;
  report.statistics.low_01_percent_fps = -1.0;
  report.statistics.average_speed = 0.9;
  report.statistics.target_frame_ms = 16.7;
  report.zone_ms_per_frame[static_cast<std::size_t>(Zone::GpuWait)] = sync_ms;
  report.zone_ms_per_frame[static_cast<std::size_t>(Zone::GuestCpu)] = guest_ms;
  report.counters[static_cast<std::size_t>(Counter::DrawCalls)] = 4200;
  FrameRecord frame;
  frame.time_s = 0.02;
  frame.frame_ms = 20.0;
  frame.fps = 50.0;
  frame.speed = 0.9;
  frame.zone_ms[static_cast<std::size_t>(Zone::GpuWait)] = static_cast<float>(sync_ms);
  frame.counters[static_cast<std::size_t>(Counter::DrawCalls)] = 42;
  report.frames.push_back(frame);
  Event event;
  event.time_s = 0.5;
  event.type = EventType::PipelineCompilation;
  event.duration_ms = 8.25;
  event.detail = "pipeline";
  report.events.push_back(event);
  ThreadRecord thread;
  thread.name = "CPU";
  thread.id = 7;
  thread.cpu_seconds = 1.8;
  thread.utilization = 0.9;
  report.threads.push_back(thread);
  HotspotRecord hotspot;
  hotspot.guest_pc = 0x801B8024u;
  hotspot.symbol = "HSD_TExpMakeDag";
  hotspot.samples = 1842;
  hotspot.percent = 12.8;
  report.hotspots.push_back(hotspot);
  report.log_lines.push_back("audio backend: cubeb");
  report.analysis = Analyze(AnalysisInput{});
  return report;
}

void TestReportRoundTrip()
{
  const fs::path path = fs::temp_directory_path() / "moderngekko-diag-roundtrip.mgdiag";
  fs::remove(path);
  const Report original = MakeReport("GTEST1", std::string(64, 'c').c_str(), 9.1, 2.8);
  Check(WriteReport(original, path).ok, "a report writes");

  const ReadResult read = ReadReport(path);
  Check(read.ok, "a report reads back");
  if (read.ok)
  {
    const Report& copy = read.report;
    Check(copy.schema_version == kSchemaVersion, "the schema version round-trips");
    Check(copy.game.disc_id == "GTEST1", "the disc id round-trips");
    Check(copy.game.entry_point == 0x80003100u, "the entry point round-trips");
    Check(copy.module.sha256 == std::string(64, 'c'), "the module hash round-trips");
    Check(copy.build.git_commit == "0123456789abcdef", "the build commit round-trips");
    Check(copy.graphics.internal_resolution_scale == 2, "graphics settings round-trip");
    Check(copy.statistics.p999_frame_ms < 0.0, "an unavailable percentile stays unavailable");
    Check(copy.frames.size() == 1, "frames round-trip");
    Check(copy.frames.front().counters[static_cast<std::size_t>(Counter::DrawCalls)] == 42,
          "per-frame counters round-trip");
    Check(copy.events.size() == 1 && copy.events.front().type == EventType::PipelineCompilation,
          "events round-trip");
    Check(copy.threads.size() == 1 && copy.threads.front().name == "CPU", "threads round-trip");
    Check(copy.hotspots.size() == 1 && copy.hotspots.front().symbol == "HSD_TExpMakeDag" &&
              copy.hotspots.front().guest_pc == 0x801B8024u,
          "hotspots round-trip");
    Check(copy.log_lines.size() == 1, "log lines round-trip");
  }

  // The archive must actually contain the documented files.
  std::vector<detail::ArchiveEntry> entries;
  std::string error;
  Check(detail::ReadArchive(path, &entries, &error), "the report is a readable archive");
  const auto has = [&](const char* name) {
    for (const detail::ArchiveEntry& entry : entries)
    {
      if (entry.name == name)
        return true;
    }
    return false;
  };
  for (const char* name : {"report.json", "system.json", "build.json", "game.json", "config.json",
                           "summary.json", "frames.csv", "threads.csv", "events.jsonl",
                           "counters.json", "hotspots.csv", "runtime.log", "README.txt"})
    Check(has(name), "the archive contains its documented files");

  for (const detail::ArchiveEntry& entry : entries)
  {
    if (entry.name != "frames.csv")
      continue;
    Check(entry.data.rfind("time_s,frame_ms,fps,vps,speed,", 0) == 0,
          "frames.csv starts with the fixed columns");
    Check(entry.data.find("GuestCpu_ms") != std::string::npos,
          "frames.csv names every zone column");
    Check(entry.data.find("draw_calls") != std::string::npos,
          "frames.csv names every counter column");
  }
  fs::remove(path);
}

void TestFutureSchemaIsRejected()
{
  const fs::path path = fs::temp_directory_path() / "moderngekko-diag-future.mgdiag";
  fs::remove(path);
  const std::vector<detail::ArchiveEntry> entries = {
      {"report.json", "{\"schema_version\": 9999}"}};
  std::string error;
  Check(detail::WriteArchive(path, entries, &error), "a synthetic report writes");
  const ReadResult read = ReadReport(path);
  Check(!read.ok, "a newer schema version is refused rather than misread");
  Check(read.error.find("newer") != std::string::npos, "the refusal explains itself");
  fs::remove(path);
}

void TestComparison()
{
  const Report a = MakeReport("GTEST1", std::string(64, 'c').c_str(), 0.3, 2.8);
  Report b = MakeReport("GTEST1", std::string(64, 'c').c_str(), 9.1, 7.6);
  b.statistics.average_fps = 42.8;
  b.statistics.average_speed = 0.71;
  b.system.cpu_model = "Slower CPU";

  const Comparison matching = CompareReports(a, b);
  Check(matching.mismatches.empty(), "matching captures report no mismatches");
  Check(matching.dominant_difference.find("Synchronization") != std::string::npos,
        "the largest per-frame delta is named as the dominant difference");
  Check(matching.dominant_difference.find("+8.80") != std::string::npos,
        "the dominant difference reports the delta in milliseconds");
  const std::string rendered = RenderComparison(a, b, matching);
  Check(rendered.find("SYSTEM A") != std::string::npos &&
            rendered.find("SYSTEM B") != std::string::npos,
        "the comparison renders both columns");
  Check(rendered.find("Interpreter fallbacks") != std::string::npos,
        "the comparison includes fallback counts");

  Report other = MakeReport("GTEST2", std::string(64, 'd').c_str(), 0.3, 2.8);
  other.build.git_commit = "feedfacefeedface";
  other.graphics.backend = "OpenGL";
  other.graphics.internal_resolution_scale = 4;
  const Comparison mismatched = CompareReports(a, other);
  Check(mismatched.mismatches.size() >= 5, "every identity difference is flagged");
  const auto mentions = [&](const char* text) {
    for (const std::string& line : mismatched.mismatches)
    {
      if (line.find(text) != std::string::npos)
        return true;
    }
    return false;
  };
  Check(mentions("disc id"), "a different disc id is flagged");
  Check(mentions("recomp module sha256"), "a different recomp module is flagged");
  Check(mentions("ModernGekko commit"), "a different ModernGekko build is flagged");
  Check(mentions("graphics backend"), "a different graphics backend is flagged");
  Check(mentions("internal resolution scale"), "a different internal resolution is flagged");
  Check(RenderComparison(a, other, mismatched).find("WARNING") != std::string::npos,
        "a mismatched comparison warns before showing numbers");
}

void TestRendering()
{
  const Report report = MakeReport("GTEST1", std::string(64, 'c').c_str(), 9.1, 2.8);
  const std::string info = RenderInfo(report);
  Check(info.find("GTEST1") != std::string::npos, "info names the disc id");
  Check(info.find(std::string(64, 'b')) != std::string::npos, "info shows the DOL hash");
  const std::string summary = RenderSummary(report);
  Check(summary.find("LIKELY BOTTLENECK") != std::string::npos, "summary states a verdict");
  Check(summary.find("CONFIDENCE") != std::string::npos, "summary states its confidence");
  Check(summary.find("HSD_TExpMakeDag") != std::string::npos, "summary lists guest hotspots");
  Check(summary.find("draw_calls") != std::string::npos, "summary lists non-zero counters");
}

void TestSystemAndBuildIdentity()
{
  const SystemInfo system = CollectSystemInfo();
  Check(!system.cpu_model.empty(), "a CPU model is reported");
  Check(system.logical_processors > 0, "logical processors are reported");
  Check(system.physical_cores > 0, "physical cores are reported");
  Check(!system.architecture.empty(), "an architecture is reported");
  Check(!system.os_name.empty(), "an operating system is reported");

  const BuildInfo build = CurrentBuildInfo();
  Check(!build.compiler.empty(), "a compiler is reported");
  Check(!build.compiler_version.empty(), "a compiler version is reported");
  Check(!build.host_architecture.empty(), "a host architecture is reported");
  Check(!build.cpu_backend.empty(), "a CPU backend is reported");
}
}  // namespace

int main()
{
  TestSynchronizationVerdict();
  TestGuestCpuVerdict();
  TestRendererVerdict();
  TestGpuVerdict();
  TestStaticRecompVerdict();
  TestShaderHitchVerdict();
  TestFullSpeedAndShortCaptures();
  TestReportRoundTrip();
  TestFutureSchemaIsRejected();
  TestComparison();
  TestRendering();
  TestSystemAndBuildIdentity();
  if (g_failures != 0)
    std::fprintf(stderr, "%d diagnostics report check(s) failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
