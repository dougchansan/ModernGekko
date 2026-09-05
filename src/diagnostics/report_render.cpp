#include "moderngekko/diagnostics_report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace moderngekko::diagnostics
{
namespace
{
std::string Fixed(double value, int decimals)
{
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

std::string Optional(double value, int decimals, const char* suffix)
{
  if (value < 0.0)
    return "n/a";
  return Fixed(value, decimals) + suffix;
}

std::string Pad(std::string text, std::size_t width)
{
  if (text.size() < width)
    text.append(width - text.size(), ' ');
  return text;
}

std::string PadLeft(std::string text, std::size_t width)
{
  if (text.size() < width)
    text.insert(text.begin(), width - text.size(), ' ');
  return text;
}

std::string Bytes(std::uint64_t value)
{
  if (value == 0)
    return "unavailable";
  const double gib = static_cast<double>(value) / (1024.0 * 1024.0 * 1024.0);
  return Fixed(gib, 2) + " GiB";
}

double ZoneOf(const Report& report, Zone zone)
{
  return report.zone_ms_per_frame[static_cast<std::size_t>(zone)];
}

std::uint64_t CounterOf(const Report& report, Counter counter)
{
  return report.counters[static_cast<std::size_t>(counter)];
}

double RendererMs(const Report& report)
{
  return ZoneOf(report, Zone::RendererSubmission) + ZoneOf(report, Zone::Present);
}

double GuestMs(const Report& report)
{
  return ZoneOf(report, Zone::GuestCpu) + ZoneOf(report, Zone::StaticRecompDispatch) +
         ZoneOf(report, Zone::InterpreterFallback);
}

double SyncMs(const Report& report)
{
  return ZoneOf(report, Zone::GpuWait) + ZoneOf(report, Zone::Synchronization);
}

void CheckMismatch(std::vector<std::string>* out, std::string_view label,
                   const std::string& a, const std::string& b)
{
  if (a == b)
    return;
  const auto shown = [](const std::string& value) {
    return value.empty() ? std::string("<none>") : value;
  };
  out->push_back(std::string(label) + ": " + shown(a) + " vs " + shown(b));
}
}  // namespace

std::string RenderInfo(const Report& report)
{
  std::ostringstream out;
  out << "ModernGekko diagnostics report\n";
  out << "  schema version   " << report.schema_version << '\n';
  out << "  created (UTC)    " << report.created_utc << '\n';
  out << "  capture kind     " << report.capture_kind << '\n';
  out << "  level            " << LevelName(report.level) << '\n';
  out << "  anonymized       " << (report.anonymized ? "yes" : "no") << '\n';
  out << "\nGame\n";
  out << "  title            " << report.game.title << '\n';
  out << "  disc id          " << report.game.disc_id << '\n';
  out << "  platform         " << report.game.platform << '\n';
  out << "  main.dol         " << report.game.dol_sha256 << '\n';
  if (!report.game.rel_sha256.empty())
    out << "  main.rel         " << report.game.rel_sha256 << '\n';
  if (!report.game.assets_sha256.empty())
    out << "  assets           " << report.game.assets_sha256 << '\n';
  out << "  recomp module    " << report.module.kind;
  if (!report.module.file_name.empty())
    out << ' ' << report.module.file_name;
  out << '\n';
  if (!report.module.sha256.empty())
    out << "  module sha256    " << report.module.sha256 << '\n';
  out << "\nBuild\n";
  out << "  version          " << report.build.version << '\n';
  out << "  commit           " << report.build.git_commit << " (" << report.build.git_state << ")\n";
  out << "  configuration    " << report.build.configuration
      << (report.build.link_time_optimization ? " +LTO" : "") << '\n';
  out << "  compiler         " << report.build.compiler << ' ' << report.build.compiler_version
      << '\n';
  out << "  cpu backend      " << report.build.cpu_backend << '\n';
  out << "\nSystem\n";
  out << "  cpu              " << report.system.cpu_model << '\n';
  out << "  cores            " << report.system.physical_cores << " physical / "
      << report.system.logical_processors << " logical\n";
  out << "  architecture     " << report.system.architecture << '\n';
  out << "  instruction sets ";
  for (std::size_t i = 0; i < report.system.instruction_sets.size(); ++i)
    out << (i == 0 ? "" : " ") << report.system.instruction_sets[i];
  out << '\n';
  out << "  memory           " << Bytes(report.system.total_physical_memory_bytes) << '\n';
  out << "  os               " << report.system.os_name << ' ' << report.system.os_version << " ("
      << report.system.os_architecture << ")\n";
  out << "  graphics         " << report.graphics.backend;
  if (!report.graphics.adapter.empty())
    out << " / " << report.graphics.adapter;
  out << '\n';
  if (!report.graphics.driver.empty())
    out << "  driver           " << report.graphics.driver << '\n';
  out << "  internal scale   " << report.graphics.internal_resolution_scale << "x\n";
  return out.str();
}

std::string RenderSummary(const Report& report)
{
  const FrameStatistics& stats = report.statistics;
  std::ostringstream out;
  out << "Capture: " << stats.frame_count << " frames over " << Fixed(stats.duration_s, 2)
      << " s at level " << LevelName(report.level) << "\n\n";
  out << "Frame timing\n";
  out << "  average fps      " << Fixed(stats.average_fps, 2) << '\n';
  out << "  average frame    " << Fixed(stats.average_frame_ms, 2) << " ms\n";
  out << "  median frame     " << Fixed(stats.median_frame_ms, 2) << " ms\n";
  out << "  p90 / p95 / p99  " << Fixed(stats.p90_frame_ms, 2) << " / "
      << Fixed(stats.p95_frame_ms, 2) << " / " << Fixed(stats.p99_frame_ms, 2) << " ms\n";
  out << "  p99.9            " << Optional(stats.p999_frame_ms, 2, " ms") << '\n';
  out << "  max frame        " << Fixed(stats.max_frame_ms, 2) << " ms\n";
  out << "  1% low           " << Fixed(stats.low_1_percent_fps, 2) << " fps\n";
  out << "  0.1% low         " << Optional(stats.low_01_percent_fps, 2, " fps") << '\n';
  out << "  over budget      " << Fixed(stats.over_budget_percent, 1) << "% of frames above "
      << Fixed(stats.target_frame_ms, 2) << " ms\n";
  out << "  emulation speed  " << Fixed(stats.average_speed * 100.0, 1) << "% average, "
      << Fixed(stats.min_speed * 100.0, 1) << "% minimum\n";
  out << "  vblanks/s        " << Fixed(stats.average_vps, 2) << '\n';

  out << "\nMilliseconds per frame by zone\n";
  std::vector<std::pair<double, std::string>> zones;
  for (std::size_t i = 0; i < kZoneCount; ++i)
  {
    if (report.zone_ms_per_frame[i] <= 0.0)
      continue;
    zones.emplace_back(report.zone_ms_per_frame[i], ZoneName(static_cast<Zone>(i)));
  }
  std::sort(zones.begin(), zones.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  if (zones.empty())
  {
    out << "  (no zone timings recorded at this diagnostics level)\n";
  }
  else
  {
    for (const auto& [value, name] : zones)
      out << "  " << Pad(name, 24) << PadLeft(Fixed(value, 3), 8) << " ms\n";
  }

  out << "\nCounters (capture totals, non-zero only)\n";
  bool any_counter = false;
  for (std::size_t i = 0; i < kCounterCount; ++i)
  {
    if (report.counters[i] == 0)
      continue;
    any_counter = true;
    out << "  " << Pad(CounterName(static_cast<Counter>(i)), 36)
        << PadLeft(std::to_string(report.counters[i]), 14) << '\n';
  }
  if (!any_counter)
    out << "  (none)\n";

  if (!report.hotspots.empty())
  {
    out << "\nGuest hotspots\n";
    const std::size_t shown = std::min<std::size_t>(report.hotspots.size(), 15);
    for (std::size_t i = 0; i < shown; ++i)
    {
      const HotspotRecord& hotspot = report.hotspots[i];
      char address[16];
      std::snprintf(address, sizeof(address), "0x%08X", hotspot.guest_pc);
      out << "  " << address << "  " << Pad(hotspot.symbol.empty() ? "?" : hotspot.symbol, 32)
          << PadLeft(std::to_string(hotspot.samples), 8) << PadLeft(Fixed(hotspot.percent, 2), 8)
          << "%\n";
    }
  }

  if (!report.events.empty())
  {
    out << "\nEvents\n";
    const std::size_t shown = std::min<std::size_t>(report.events.size(), 20);
    for (std::size_t i = 0; i < shown; ++i)
    {
      const Event& event = report.events[i];
      out << "  t=" << PadLeft(Fixed(event.time_s, 2), 8) << "s  "
          << Pad(EventTypeName(event.type), 22) << PadLeft(Fixed(event.duration_ms, 2), 9)
          << " ms  " << event.detail << '\n';
    }
    if (report.events.size() > shown)
      out << "  ... " << (report.events.size() - shown) << " more\n";
  }

  out << "\nLIKELY BOTTLENECK:\n";
  if (report.analysis.findings.empty())
  {
    out << "  " << (report.analysis.headline.empty() ? "unknown" : report.analysis.headline) << '\n';
    return out.str();
  }
  const Finding& top = report.analysis.findings.front();
  out << "  " << (report.analysis.headline.empty() ? ClassificationName(top.classification)
                                                   : report.analysis.headline)
      << "\nCONFIDENCE:\n  " << ConfidenceName(top.confidence) << "\nEVIDENCE:\n";
  for (const std::string& line : top.evidence)
    out << "  - " << line << '\n';
  if (report.analysis.findings.size() > 1)
  {
    out << "\nOther candidates\n";
    for (std::size_t i = 1; i < report.analysis.findings.size(); ++i)
    {
      const Finding& finding = report.analysis.findings[i];
      out << "  " << Pad(ClassificationName(finding.classification), 40) << ' '
          << ConfidenceName(finding.confidence) << '\n';
    }
  }
  return out.str();
}

Comparison CompareReports(const Report& a, const Report& b)
{
  Comparison comparison;
  std::vector<std::string>& mismatches = comparison.mismatches;
  if (a.schema_version != b.schema_version)
  {
    mismatches.push_back("schema version: " + std::to_string(a.schema_version) + " vs " +
                         std::to_string(b.schema_version));
  }
  CheckMismatch(&mismatches, "disc id", a.game.disc_id, b.game.disc_id);
  CheckMismatch(&mismatches, "main.dol sha256", a.game.dol_sha256, b.game.dol_sha256);
  CheckMismatch(&mismatches, "main.rel sha256", a.game.rel_sha256, b.game.rel_sha256);
  CheckMismatch(&mismatches, "assets sha256", a.game.assets_sha256, b.game.assets_sha256);
  CheckMismatch(&mismatches, "recomp module sha256", a.module.sha256, b.module.sha256);
  CheckMismatch(&mismatches, "ModernGekko commit", a.build.git_commit, b.build.git_commit);
  CheckMismatch(&mismatches, "build configuration", a.build.configuration, b.build.configuration);
  CheckMismatch(&mismatches, "graphics backend", a.graphics.backend, b.graphics.backend);
  if (a.graphics.internal_resolution_scale != b.graphics.internal_resolution_scale)
  {
    mismatches.push_back("internal resolution scale: " +
                         std::to_string(a.graphics.internal_resolution_scale) + "x vs " +
                         std::to_string(b.graphics.internal_resolution_scale) + "x");
  }
  if (a.level != b.level)
  {
    mismatches.push_back("diagnostics level: " + std::string(LevelName(a.level)) + " vs " +
                         LevelName(b.level));
  }

  const auto row = [&](std::string label, double left, double right, std::string unit,
                       bool higher_is_worse, bool is_zone = false) {
    comparison.rows.push_back(ComparisonRow{std::move(label), left, right, std::move(unit),
                                            higher_is_worse, is_zone});
  };
  row("Average speed", a.statistics.average_speed * 100.0, b.statistics.average_speed * 100.0, "%",
      false);
  row("Average FPS", a.statistics.average_fps, b.statistics.average_fps, "", false);
  row("1% low FPS", a.statistics.low_1_percent_fps, b.statistics.low_1_percent_fps, "", false);
  row("P99 frame", a.statistics.p99_frame_ms, b.statistics.p99_frame_ms, "ms", true);
  row("Guest CPU", GuestMs(a), GuestMs(b), "ms", true, true);
  row("GX", ZoneOf(a, Zone::GxCommandProcessor), ZoneOf(b, Zone::GxCommandProcessor), "ms", true,
      true);
  row("Vertex conversion", ZoneOf(a, Zone::VertexLoader), ZoneOf(b, Zone::VertexLoader), "ms", true,
      true);
  row("Texture decoding", ZoneOf(a, Zone::TextureDecoder), ZoneOf(b, Zone::TextureDecoder), "ms",
      true, true);
  row("Renderer CPU", RendererMs(a), RendererMs(b), "ms", true, true);
  row("GPU execution", ZoneOf(a, Zone::GpuExecution), ZoneOf(b, Zone::GpuExecution), "ms", true,
      true);
  row("Synchronization", SyncMs(a), SyncMs(b), "ms", true, true);
  row("Interpreter fallbacks", static_cast<double>(CounterOf(a, Counter::InterpreterFallbacks)),
      static_cast<double>(CounterOf(b, Counter::InterpreterFallbacks)), "", true);
  row("Shader compilations", static_cast<double>(CounterOf(a, Counter::ShaderCompilations)),
      static_cast<double>(CounterOf(b, Counter::ShaderCompilations)), "", true);

  // Only subsystem timings explain a difference; a P99 delta restates it.
  double worst = 0.0;
  for (const ComparisonRow& entry : comparison.rows)
  {
    if (!entry.is_zone)
      continue;
    const double delta = entry.b - entry.a;
    if (std::abs(delta) > std::abs(worst))
    {
      worst = delta;
      comparison.dominant_difference =
          entry.label + (delta >= 0.0 ? " +" : " ") + Fixed(delta, 2) + " ms/frame";
    }
  }
  if (comparison.dominant_difference.empty())
  {
    const double speed_delta =
        (b.statistics.average_speed - a.statistics.average_speed) * 100.0;
    comparison.dominant_difference =
        "No per-zone timings to compare; average speed differs by " + Fixed(speed_delta, 1) + "%";
  }
  return comparison;
}

std::string RenderComparison(const Report& a, const Report& b, const Comparison& comparison)
{
  std::ostringstream out;
  constexpr std::size_t kLabelWidth = 24;
  constexpr std::size_t kColumnWidth = 26;
  const auto clip = [](const std::string& text) {
    return text.size() <= kColumnWidth - 2 ? text : text.substr(0, kColumnWidth - 2);
  };
  const auto adapter = [&](const Report& report) {
    return report.graphics.adapter.empty() ? report.graphics.backend : report.graphics.adapter;
  };
  out << Pad("", kLabelWidth) << Pad("SYSTEM A", kColumnWidth) << "SYSTEM B\n";
  out << Pad("CPU", kLabelWidth) << Pad(clip(a.system.cpu_model), kColumnWidth)
      << clip(b.system.cpu_model) << '\n';
  out << Pad("GPU", kLabelWidth) << Pad(clip(adapter(a)), kColumnWidth) << clip(adapter(b))
      << "\n\n";

  if (comparison.mismatches.empty())
  {
    out << "Reports match on game content, recomp module, build and graphics settings.\n\n";
  }
  else
  {
    out << "WARNING: these reports do not describe the same configuration.\n";
    for (const std::string& mismatch : comparison.mismatches)
      out << "  ! " << mismatch << '\n';
    out << "Performance differences below may be caused by these mismatches.\n\n";
  }

  for (const ComparisonRow& row : comparison.rows)
  {
    const int decimals = row.unit == "ms" ? 2 : (row.unit == "%" ? 1 : 2);
    std::string left = Fixed(row.a, decimals);
    std::string right = Fixed(row.b, decimals);
    if (!row.unit.empty())
    {
      left += ' ' + row.unit;
      right += ' ' + row.unit;
    }
    out << Pad(row.label, kLabelWidth) << Pad(left, kColumnWidth) << right << '\n';
  }

  out << "\nDOMINANT DIFFERENCE:\n  " << comparison.dominant_difference << '\n';
  if (!a.analysis.headline.empty() || !b.analysis.headline.empty())
  {
    out << "\nVERDICTS:\n";
    out << "  A: " << (a.analysis.headline.empty() ? "unknown" : a.analysis.headline) << '\n';
    out << "  B: " << (b.analysis.headline.empty() ? "unknown" : b.analysis.headline) << '\n';
  }
  return out.str();
}
}  // namespace moderngekko::diagnostics
