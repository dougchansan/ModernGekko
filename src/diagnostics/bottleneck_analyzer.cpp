#include "moderngekko/diagnostics_report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace moderngekko::diagnostics
{
namespace
{
double Percentile(const std::vector<double>& sorted, double quantile)
{
  if (sorted.empty())
    return 0.0;
  const double rank = std::ceil(quantile * static_cast<double>(sorted.size()));
  auto index = static_cast<std::size_t>(rank);
  if (index == 0)
    index = 1;
  if (index > sorted.size())
    index = sorted.size();
  return sorted[index - 1];
}

// Mean of the slowest `fraction` of frames, expressed as frames per second.
double LowFps(const std::vector<double>& sorted, double fraction)
{
  if (sorted.empty())
    return 0.0;
  auto count = static_cast<std::size_t>(
      std::floor(fraction * static_cast<double>(sorted.size())));
  if (count == 0)
    count = 1;
  const double total =
      std::accumulate(sorted.end() - static_cast<std::ptrdiff_t>(count), sorted.end(), 0.0);
  const double mean = total / static_cast<double>(count);
  return mean > 0.0 ? 1000.0 / mean : 0.0;
}

double Share(double part, double whole)
{
  return whole > 0.0 ? part / whole : 0.0;
}

std::string Ms(double value)
{
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f ms", value);
  return buffer;
}

std::string Percent(double fraction)
{
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f%%", fraction * 100.0);
  return buffer;
}

std::string Num(double value, int decimals)
{
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

double ZoneMs(const AnalysisInput& input, Zone zone)
{
  return input.zone_ms_per_frame[static_cast<std::size_t>(zone)];
}

std::uint64_t CounterValue(const AnalysisInput& input, Counter counter)
{
  return input.counters[static_cast<std::size_t>(counter)];
}
}  // namespace

const char* ClassificationName(Classification value)
{
  switch (value)
  {
  case Classification::Unknown: return "unknown";
  case Classification::GuestCpuBound: return "CPU / guest execution bound";
  case Classification::StaticRecompSlowPathBound: return "StaticRecomp slow-path bound";
  case Classification::GpuBound: return "GPU bound";
  case Classification::CpuGpuSynchronizationBound: return "CPU <-> GPU synchronization bound";
  case Classification::RendererCpuBound: return "Renderer CPU bound";
  case Classification::ShaderCompilationHitching: return "Shader/pipeline compilation hitching";
  case Classification::TextureOrEfbTransferBound: return "Texture/EFB transfer bound";
  case Classification::AudioOrTimingWait: return "Audio/timing wait";
  case Classification::NetplayWait: return "Netplay wait";
  case Classification::ThreadContention: return "Thread contention";
  case Classification::PossibleHardwareThrottling: return "Possible hardware throttling";
  case Classification::InsufficientEvidence: return "Insufficient evidence";
  }
  return "unknown";
}

const char* ConfidenceName(Confidence value)
{
  switch (value)
  {
  case Confidence::Low: return "LOW";
  case Confidence::Medium: return "MEDIUM";
  case Confidence::High: return "HIGH";
  }
  return "LOW";
}

FrameStatistics ComputeFrameStatistics(const std::vector<FrameRecord>& frames,
                                       double target_frame_ms)
{
  FrameStatistics stats;
  stats.target_frame_ms = target_frame_ms > 0.0 ? target_frame_ms : 16.7;
  stats.frame_count = frames.size();
  if (frames.empty())
    return stats;

  std::vector<double> times;
  times.reserve(frames.size());
  double speed_total = 0.0;
  double vps_total = 0.0;
  double min_speed = frames.front().speed;
  std::size_t over_budget = 0;
  for (const FrameRecord& frame : frames)
  {
    times.push_back(frame.frame_ms);
    speed_total += frame.speed;
    vps_total += frame.vps;
    min_speed = std::min(min_speed, frame.speed);
    if (frame.frame_ms > stats.target_frame_ms)
      ++over_budget;
  }
  stats.duration_s = frames.back().time_s - frames.front().time_s;
  if (stats.duration_s <= 0.0)
    stats.duration_s = std::accumulate(times.begin(), times.end(), 0.0) / 1000.0;

  const double count = static_cast<double>(frames.size());
  stats.average_frame_ms = std::accumulate(times.begin(), times.end(), 0.0) / count;
  stats.average_fps = stats.average_frame_ms > 0.0 ? 1000.0 / stats.average_frame_ms : 0.0;
  stats.average_speed = speed_total / count;
  stats.average_vps = vps_total / count;
  stats.min_speed = min_speed;
  stats.over_budget_percent = 100.0 * static_cast<double>(over_budget) / count;

  std::sort(times.begin(), times.end());
  stats.median_frame_ms = Percentile(times, 0.50);
  stats.p90_frame_ms = Percentile(times, 0.90);
  stats.p95_frame_ms = Percentile(times, 0.95);
  stats.p99_frame_ms = Percentile(times, 0.99);
  stats.p999_frame_ms = times.size() >= 1000 ? Percentile(times, 0.999) : -1.0;
  stats.max_frame_ms = times.back();
  stats.low_1_percent_fps = LowFps(times, 0.01);
  stats.low_01_percent_fps = times.size() >= 1000 ? LowFps(times, 0.001) : -1.0;
  return stats;
}

Analysis Analyze(const AnalysisInput& input)
{
  Analysis analysis;
  const FrameStatistics& stats = input.frames;
  const double frame_ms = stats.average_frame_ms;

  const double guest_ms = ZoneMs(input, Zone::GuestCpu) + ZoneMs(input, Zone::StaticRecompDispatch) +
                          ZoneMs(input, Zone::InterpreterFallback);
  const double renderer_ms = ZoneMs(input, Zone::RendererSubmission) +
                             ZoneMs(input, Zone::GxCommandProcessor) +
                             ZoneMs(input, Zone::VertexLoader);
  const double gpu_ms = ZoneMs(input, Zone::GpuExecution);
  const double wait_ms = ZoneMs(input, Zone::GpuWait) + ZoneMs(input, Zone::Synchronization);
  const double transfer_ms = ZoneMs(input, Zone::TextureDecoder) + ZoneMs(input, Zone::Memory);
  const double audio_ms = ZoneMs(input, Zone::Audio) + ZoneMs(input, Zone::Dsp) +
                          ZoneMs(input, Zone::Scheduler);
  const double netplay_ms = ZoneMs(input, Zone::Netplay);
  const double shader_ms = ZoneMs(input, Zone::ShaderGeneration) + ZoneMs(input, Zone::PipelineCreation);
  double measured_ms = 0.0;
  for (std::size_t i = 0; i < kZoneCount; ++i)
    measured_ms += input.zone_ms_per_frame[i];

  // Findings are scored so the ordering is a pure function of the telemetry.
  struct Scored
  {
    double score = 0.0;
    Finding finding;
  };
  std::vector<Scored> scored;
  const auto add = [&](double score, Classification classification, Confidence confidence,
                       std::vector<std::string> evidence) {
    scored.push_back({score, Finding{classification, confidence, std::move(evidence)}});
  };

  if (stats.frame_count < 10)
  {
    analysis.findings.push_back(
        Finding{Classification::InsufficientEvidence, Confidence::High,
                {"Capture contains " + std::to_string(stats.frame_count) +
                 " frames; at least 10 are needed for a verdict."}});
    analysis.headline = ClassificationName(Classification::InsufficientEvidence);
    return analysis;
  }

  const double coverage = Share(measured_ms, frame_ms);
  if (input.level == Level::Basic || coverage < 0.30)
  {
    std::vector<std::string> evidence;
    evidence.push_back("Diagnostics level: " + std::string(LevelName(input.level)));
    evidence.push_back("Instrumented time accounts for " + Percent(coverage) +
                       " of the average frame; subsystem attribution is unreliable below 30%.");
    evidence.push_back("Average frame time: " + Ms(frame_ms));
    evidence.push_back("Average emulation speed: " + Percent(stats.average_speed));
    add(0.05, Classification::InsufficientEvidence, Confidence::Medium, std::move(evidence));
  }

  // Shader and pipeline compilation correlated against long frames.
  double compile_ms_total = 0.0;
  std::size_t compile_events = 0;
  double worst_compile_ms = 0.0;
  double worst_compile_time = 0.0;
  for (const Event& event : input.events)
  {
    if (event.type != EventType::ShaderCompilation && event.type != EventType::PipelineCompilation)
      continue;
    ++compile_events;
    compile_ms_total += event.duration_ms;
    if (event.duration_ms > worst_compile_ms)
    {
      worst_compile_ms = event.duration_ms;
      worst_compile_time = event.time_s;
    }
  }
  if (compile_events > 0 && stats.duration_s > 0.0)
  {
    const double compile_share = Share(compile_ms_total / 1000.0, stats.duration_s);
    const bool hitching = compile_share > 0.02 || worst_compile_ms > 2.0 * stats.median_frame_ms;
    if (hitching)
    {
      std::vector<std::string> evidence;
      evidence.push_back(std::to_string(compile_events) +
                         " shader/pipeline compilations during the capture, " +
                         Ms(compile_ms_total) + " total");
      evidence.push_back("Longest compilation: " + Ms(worst_compile_ms) + " at t=" +
                         Num(worst_compile_time, 2) + "s");
      evidence.push_back("Median frame time: " + Ms(stats.median_frame_ms) +
                         ", maximum frame time: " + Ms(stats.max_frame_ms));
      evidence.push_back("Compilation consumed " + Percent(compile_share) + " of the capture");
      const Confidence confidence =
          worst_compile_ms > 2.0 * stats.median_frame_ms ? Confidence::High : Confidence::Medium;
      add(0.5 + compile_share, Classification::ShaderCompilationHitching, confidence,
          std::move(evidence));
    }
  }

  if (frame_ms > 0.0 && coverage >= 0.30)
  {
    const double wait_share = Share(wait_ms, frame_ms);
    if (wait_share >= 0.20 && wait_ms > guest_ms && wait_ms > renderer_ms)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average guest CPU: " + Ms(guest_ms));
      evidence.push_back("Average GPU execution: " +
                         (gpu_ms > 0.0 ? Ms(gpu_ms) : std::string("not reported by this backend")));
      evidence.push_back("Average GPU wait: " + Ms(ZoneMs(input, Zone::GpuWait)));
      evidence.push_back("Average other synchronization: " + Ms(ZoneMs(input, Zone::Synchronization)));
      evidence.push_back("Waits consumed " + Percent(wait_share) + " of frame time");
      evidence.push_back("Interpreter fallbacks: " +
                         std::to_string(CounterValue(input, Counter::InterpreterFallbacks)));
      evidence.push_back("Shader compilations during capture: " + std::to_string(compile_events));
      add(0.4 + wait_share, Classification::CpuGpuSynchronizationBound,
          wait_share >= 0.35 ? Confidence::High : Confidence::Medium, std::move(evidence));
    }

    const double gpu_share = Share(gpu_ms, frame_ms);
    if (gpu_ms > 0.0 && gpu_share >= 0.40 && gpu_ms > guest_ms && gpu_ms > renderer_ms)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average GPU execution (timestamp queries): " + Ms(gpu_ms));
      evidence.push_back("Average guest CPU: " + Ms(guest_ms));
      evidence.push_back("Average renderer CPU submission: " + Ms(renderer_ms));
      evidence.push_back("GPU execution consumed " + Percent(gpu_share) + " of frame time");
      add(0.4 + gpu_share, Classification::GpuBound,
          gpu_share >= 0.60 ? Confidence::High : Confidence::Medium, std::move(evidence));
    }

    const double guest_share = Share(guest_ms, frame_ms);
    if (guest_share >= 0.40 && guest_ms > renderer_ms && guest_ms > wait_ms)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average guest CPU: " + Ms(guest_ms));
      evidence.push_back("Guest execution consumed " + Percent(guest_share) + " of frame time");
      evidence.push_back("Average emulation speed: " + Percent(stats.average_speed));
      evidence.push_back("StaticRecomp dispatches: " +
                         std::to_string(CounterValue(input, Counter::StaticRecompDispatches)));
      evidence.push_back("Average renderer CPU submission: " + Ms(renderer_ms));
      add(0.4 + guest_share, Classification::GuestCpuBound,
          guest_share >= 0.60 ? Confidence::High : Confidence::Medium, std::move(evidence));
    }

    const double renderer_share = Share(renderer_ms, frame_ms);
    if (renderer_share >= 0.35 && renderer_ms > guest_ms && renderer_ms > wait_ms)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average renderer CPU submission: " + Ms(renderer_ms));
      evidence.push_back("  GX command processing: " + Ms(ZoneMs(input, Zone::GxCommandProcessor)));
      evidence.push_back("  Vertex conversion: " + Ms(ZoneMs(input, Zone::VertexLoader)));
      evidence.push_back("Draw calls per frame: " +
                         std::to_string(stats.frame_count > 0
                                            ? CounterValue(input, Counter::DrawCalls) / stats.frame_count
                                            : 0));
      evidence.push_back("Renderer CPU consumed " + Percent(renderer_share) + " of frame time");
      add(0.35 + renderer_share, Classification::RendererCpuBound,
          renderer_share >= 0.55 ? Confidence::High : Confidence::Medium, std::move(evidence));
    }

    const double transfer_share = Share(transfer_ms, frame_ms);
    const std::uint64_t efb_traffic = CounterValue(input, Counter::EfbReads) +
                                      CounterValue(input, Counter::EfbCopies) +
                                      CounterValue(input, Counter::FramebufferReadbacks);
    if (transfer_share >= 0.20 || (efb_traffic > 0 && stats.frame_count > 0 &&
                                   efb_traffic / stats.frame_count >= 8))
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average texture decoding: " + Ms(ZoneMs(input, Zone::TextureDecoder)));
      evidence.push_back("EFB reads: " + std::to_string(CounterValue(input, Counter::EfbReads)) +
                         ", EFB copies: " + std::to_string(CounterValue(input, Counter::EfbCopies)) +
                         ", framebuffer readbacks: " +
                         std::to_string(CounterValue(input, Counter::FramebufferReadbacks)));
      evidence.push_back("Texture uploads: " + std::to_string(CounterValue(input, Counter::TextureUploads)) +
                         " (" + std::to_string(CounterValue(input, Counter::TextureUploadBytes)) + " bytes)");
      add(0.3 + transfer_share, Classification::TextureOrEfbTransferBound, Confidence::Medium,
          std::move(evidence));
    }

    const double audio_share = Share(audio_ms, frame_ms);
    if (audio_share >= 0.20)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average audio/DSP time: " +
                         Ms(ZoneMs(input, Zone::Audio) + ZoneMs(input, Zone::Dsp)));
      evidence.push_back("Average scheduler/timing wait: " + Ms(ZoneMs(input, Zone::Scheduler)));
      evidence.push_back("Audio underruns: " +
                         std::to_string(CounterValue(input, Counter::AudioUnderruns)));
      add(0.25 + audio_share, Classification::AudioOrTimingWait, Confidence::Medium,
          std::move(evidence));
    }

    const double netplay_share = Share(netplay_ms, frame_ms);
    if (netplay_share >= 0.10 || CounterValue(input, Counter::NetplayInputWaits) > stats.frame_count)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average netplay input wait: " + Ms(netplay_ms));
      evidence.push_back("Netplay input waits: " +
                         std::to_string(CounterValue(input, Counter::NetplayInputWaits)));
      add(0.3 + netplay_share, Classification::NetplayWait,
          netplay_share >= 0.20 ? Confidence::High : Confidence::Medium, std::move(evidence));
    }

    if (shader_ms > 0.10 * frame_ms)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Average shader generation: " + Ms(ZoneMs(input, Zone::ShaderGeneration)));
      evidence.push_back("Average pipeline creation: " + Ms(ZoneMs(input, Zone::PipelineCreation)));
      evidence.push_back("Shader cache misses: " +
                         std::to_string(CounterValue(input, Counter::ShaderCacheMisses)) + " vs hits " +
                         std::to_string(CounterValue(input, Counter::ShaderCacheHits)));
      add(0.3 + Share(shader_ms, frame_ms), Classification::ShaderCompilationHitching,
          Confidence::Medium, std::move(evidence));
    }
  }

  // StaticRecomp slow paths are judged from counters, so they work at Basic.
  const std::uint64_t dispatches = CounterValue(input, Counter::StaticRecompDispatches);
  const std::uint64_t misses = CounterValue(input, Counter::StaticRecompDispatchMisses);
  const std::uint64_t fallbacks = CounterValue(input, Counter::InterpreterFallbacks);
  const std::uint64_t slow_lookups = CounterValue(input, Counter::IndirectBranchSlowLookups);
  const std::uint64_t indirect = CounterValue(input, Counter::IndirectBranches);
  if (dispatches > 0 || fallbacks > 0)
  {
    const double miss_rate = Share(static_cast<double>(misses), static_cast<double>(dispatches + misses));
    const double slow_rate = Share(static_cast<double>(slow_lookups), static_cast<double>(indirect));
    const double fallback_per_frame =
        stats.frame_count > 0 ? static_cast<double>(fallbacks) / static_cast<double>(stats.frame_count)
                              : 0.0;
    if (miss_rate >= 0.01 || fallback_per_frame >= 100.0 || slow_rate >= 0.25 ||
        CounterValue(input, Counter::UnsupportedInstructionFallbacks) > 0)
    {
      std::vector<std::string> evidence;
      evidence.push_back("StaticRecomp dispatches: " + std::to_string(dispatches) +
                         ", dispatch misses: " + std::to_string(misses) + " (" +
                         Percent(miss_rate) + ")");
      evidence.push_back("Interpreter fallbacks: " + std::to_string(fallbacks) + " (" +
                         Num(fallback_per_frame, 1) + " per frame)");
      evidence.push_back("Unsupported-instruction fallbacks: " +
                         std::to_string(CounterValue(input, Counter::UnsupportedInstructionFallbacks)));
      evidence.push_back("Indirect branches: " + std::to_string(indirect) + ", slow lookups: " +
                         std::to_string(slow_lookups));
      evidence.push_back("Average time in interpreter fallback: " +
                         Ms(ZoneMs(input, Zone::InterpreterFallback)) + " per frame");
      const Confidence confidence =
          (miss_rate >= 0.05 || fallback_per_frame >= 1000.0) ? Confidence::High : Confidence::Medium;
      add(0.45 + miss_rate + std::min(slow_rate, 1.0) * 0.2,
          Classification::StaticRecompSlowPathBound, confidence, std::move(evidence));
    }
  }

  // A saturated worker thread on an otherwise idle machine.
  if (stats.duration_s > 0.0)
  {
    const ThreadRecord* saturated = nullptr;
    double best = 0.0;
    for (const ThreadRecord& thread : input.threads)
    {
      if (thread.utilization > best)
      {
        best = thread.utilization;
        saturated = &thread;
      }
    }
    if (saturated != nullptr && best >= 0.90 && input.threads.size() >= 2)
    {
      std::vector<std::string> evidence;
      evidence.push_back("Thread '" + saturated->name + "' used " + Percent(best) +
                         " of one core for the whole capture");
      evidence.push_back("Threads reporting CPU time: " + std::to_string(input.threads.size()));
      evidence.push_back("Average emulation speed: " + Percent(stats.average_speed));
      add(0.25 + best * 0.1, Classification::ThreadContention, Confidence::Low,
          std::move(evidence));
    }
  }

  const bool full_speed = stats.average_speed >= 0.98 && stats.over_budget_percent < 5.0;
  const double best_score =
      scored.empty()
          ? 0.0
          : std::max_element(scored.begin(), scored.end(),
                             [](const Scored& a, const Scored& b) { return a.score < b.score; })
                ->score;
  // A weak "insufficient evidence" entry must not outrank the plain fact that
  // the title is running at its intended speed.
  if (scored.empty() || (full_speed && best_score < 0.30))
  {
    std::vector<std::string> evidence;
    evidence.push_back("Average emulation speed: " + Percent(stats.average_speed));
    evidence.push_back("Average frame time: " + Ms(frame_ms) + " against a " +
                       Ms(stats.target_frame_ms) + " budget");
    evidence.push_back("Frames over budget: " + Percent(stats.over_budget_percent / 100.0));
    if (full_speed)
    {
      analysis.headline = "No bottleneck detected: running at full emulation speed";
      analysis.findings.push_back(
          Finding{Classification::Unknown, Confidence::High, std::move(evidence)});
    }
    else
    {
      analysis.findings.push_back(
          Finding{Classification::InsufficientEvidence, Confidence::Medium, std::move(evidence)});
      analysis.headline = ClassificationName(Classification::InsufficientEvidence);
    }
    return analysis;
  }

  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored& a, const Scored& b) { return a.score > b.score; });
  for (Scored& entry : scored)
    analysis.findings.push_back(std::move(entry.finding));
  analysis.headline = ClassificationName(analysis.findings.front().classification);

  // A title that renders at half speed is a different problem from a title
  // that renders at 30 FPS by design; say so explicitly.
  if (stats.average_speed > 0.0 && stats.average_speed < 0.95)
  {
    analysis.findings.front().evidence.push_back(
        "Emulation speed averaged " + Percent(stats.average_speed) +
        ", so the frame rate is below the title's intended pacing.");
  }
  else if (stats.average_speed >= 0.98)
  {
    analysis.findings.front().evidence.push_back(
        "Emulation ran at " + Percent(stats.average_speed) +
        " speed, so the observed frame rate matches the title's intended pacing.");
  }
  return analysis;
}
}  // namespace moderngekko::diagnostics
