#include "diagnostics_internal.hpp"
#include "moderngekko/diagnostics_report.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace moderngekko::diagnostics
{
namespace
{
using detail::FormatDouble;
using detail::JsonWriter;

std::string HomeDirectory()
{
  const char* names[] = {"USERPROFILE", "HOME"};
  for (const char* name : names)
  {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0')
      return value;
  }
  return {};
}

std::string UserName()
{
  const char* names[] = {"USERNAME", "USER", "LOGNAME"};
  for (const char* name : names)
  {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0')
      return value;
  }
  return {};
}

std::string ReplaceAll(std::string text, std::string_view needle, std::string_view replacement)
{
  if (needle.empty())
    return text;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos)
  {
    text.replace(position, needle.size(), replacement);
    position += replacement.size();
  }
  return text;
}

// Rewrites dotted-quad addresses so netplay reports never carry peer IPs.
std::string RedactIpv4(const std::string& text)
{
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size())
  {
    std::size_t cursor = i;
    int groups = 0;
    bool valid = true;
    while (groups < 4 && valid)
    {
      std::size_t digits = 0;
      int value = 0;
      while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0)
      {
        value = value * 10 + (text[cursor] - '0');
        ++cursor;
        ++digits;
      }
      valid = digits >= 1 && digits <= 3 && value <= 255;
      ++groups;
      if (valid && groups < 4)
      {
        if (cursor < text.size() && text[cursor] == '.')
          ++cursor;
        else
          valid = false;
      }
    }
    const bool boundary_before = i == 0 || (text[i - 1] != '.' &&
                                            std::isdigit(static_cast<unsigned char>(text[i - 1])) == 0);
    const bool boundary_after =
        cursor >= text.size() || std::isdigit(static_cast<unsigned char>(text[cursor])) == 0;
    if (valid && boundary_before && boundary_after && cursor > i)
    {
      out += "<IP>";
      i = cursor;
      continue;
    }
    out += text[i];
    ++i;
  }
  return out;
}

std::string CsvField(std::string_view value)
{
  const bool needs_quotes = value.find_first_of(",\"\n\r") != std::string_view::npos;
  if (!needs_quotes)
    return std::string(value);
  std::string out = "\"";
  for (const char c : value)
  {
    if (c == '"')
      out += '"';
    out += c;
  }
  out += '"';
  return out;
}

std::string HexAddress(std::uint32_t value)
{
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out = "0x";
  for (int shift = 28; shift >= 0; shift -= 4)
    out += kHex[(value >> shift) & 0xF];
  return out;
}

void WriteBuild(JsonWriter& json, const BuildInfo& build)
{
  json.BeginObject();
  json.KeyString("version", build.version);
  json.KeyString("git_commit", build.git_commit);
  json.KeyString("git_state", build.git_state);
  json.KeyString("configuration", build.configuration);
  json.KeyString("compiler", build.compiler);
  json.KeyString("compiler_version", build.compiler_version);
  json.KeyString("host_architecture", build.host_architecture);
  json.KeyString("cpu_backend", build.cpu_backend);
  json.KeyBool("link_time_optimization", build.link_time_optimization);
  json.KeyString("build_timestamp", build.build_timestamp);
  json.EndObject();
}

void WriteSystem(JsonWriter& json, const SystemInfo& system)
{
  json.BeginObject();
  json.Key("cpu");
  json.BeginObject();
  json.KeyString("model", system.cpu_model);
  json.KeyInteger("physical_cores", static_cast<std::uint64_t>(system.physical_cores));
  json.KeyInteger("logical_processors", static_cast<std::uint64_t>(system.logical_processors));
  json.KeyString("architecture", system.architecture);
  json.Key("instruction_sets");
  json.BeginArray();
  for (const std::string& set : system.instruction_sets)
    json.String(set);
  json.EndArray();
  json.EndObject();
  json.Key("memory");
  json.BeginObject();
  json.KeyInteger("total_physical_bytes", system.total_physical_memory_bytes);
  json.KeyInteger("process_resident_bytes", system.process_resident_bytes);
  json.KeyInteger("process_peak_bytes", system.process_peak_bytes);
  json.EndObject();
  json.Key("os");
  json.BeginObject();
  json.KeyString("name", system.os_name);
  json.KeyString("version", system.os_version);
  json.KeyString("architecture", system.os_architecture);
  json.EndObject();
  json.EndObject();
}

void WriteGame(JsonWriter& json, const GameIdentity& game, const ModuleIdentity& module)
{
  json.BeginObject();
  json.KeyString("title", game.title);
  json.KeyString("disc_id", game.disc_id);
  json.KeyString("platform", game.platform);
  json.KeyString("dol_sha256", game.dol_sha256);
  json.KeyString("rel_sha256", game.rel_sha256);
  json.KeyString("assets_sha256", game.assets_sha256);
  json.KeyString("entry_point", HexAddress(game.entry_point));
  json.Key("recomp_module");
  json.BeginObject();
  json.KeyString("kind", module.kind);
  json.KeyString("file_name", module.file_name);
  json.KeyString("sha256", module.sha256);
  json.EndObject();
  json.EndObject();
}

void WriteStatistics(JsonWriter& json, const FrameStatistics& stats)
{
  json.BeginObject();
  json.KeyInteger("frame_count", static_cast<std::uint64_t>(stats.frame_count));
  json.KeyNumber("duration_s", stats.duration_s);
  json.KeyNumber("average_fps", stats.average_fps);
  json.KeyNumber("average_frame_ms", stats.average_frame_ms);
  json.KeyNumber("median_frame_ms", stats.median_frame_ms);
  json.KeyNumber("p90_frame_ms", stats.p90_frame_ms);
  json.KeyNumber("p95_frame_ms", stats.p95_frame_ms);
  json.KeyNumber("p99_frame_ms", stats.p99_frame_ms);
  json.Key("p999_frame_ms");
  if (stats.p999_frame_ms < 0.0)
    json.Null();
  else
    json.Number(stats.p999_frame_ms);
  json.KeyNumber("max_frame_ms", stats.max_frame_ms);
  json.KeyNumber("low_1_percent_fps", stats.low_1_percent_fps);
  json.Key("low_0_1_percent_fps");
  if (stats.low_01_percent_fps < 0.0)
    json.Null();
  else
    json.Number(stats.low_01_percent_fps);
  json.KeyNumber("over_budget_percent", stats.over_budget_percent);
  json.KeyNumber("target_frame_ms", stats.target_frame_ms);
  json.KeyNumber("average_speed", stats.average_speed);
  json.KeyNumber("min_speed", stats.min_speed);
  json.KeyNumber("average_vps", stats.average_vps);
  json.EndObject();
}

void WriteAnalysis(JsonWriter& json, const Analysis& analysis)
{
  json.BeginObject();
  json.KeyString("headline", analysis.headline);
  json.Key("findings");
  json.BeginArray();
  for (const Finding& finding : analysis.findings)
  {
    json.BeginObject();
    json.KeyString("classification", ClassificationName(finding.classification));
    json.KeyString("confidence", ConfidenceName(finding.confidence));
    json.Key("evidence");
    json.BeginArray();
    for (const std::string& line : finding.evidence)
      json.String(line);
    json.EndArray();
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
}

std::string FramesCsv(const Report& report)
{
  std::string out = "time_s,frame_ms,fps,vps,speed";
  for (std::size_t i = 0; i < kZoneCount; ++i)
  {
    out += ',';
    out += ZoneName(static_cast<Zone>(i));
    out += "_ms";
  }
  for (std::size_t i = 0; i < kCounterCount; ++i)
  {
    out += ',';
    out += CounterName(static_cast<Counter>(i));
  }
  out += '\n';
  for (const FrameRecord& frame : report.frames)
  {
    out += FormatDouble(frame.time_s, 4);
    out += ',' + FormatDouble(frame.frame_ms, 3);
    out += ',' + FormatDouble(frame.fps, 2);
    out += ',' + FormatDouble(frame.vps, 2);
    out += ',' + FormatDouble(frame.speed, 4);
    for (std::size_t i = 0; i < kZoneCount; ++i)
      out += ',' + FormatDouble(frame.zone_ms[i], 3);
    for (std::size_t i = 0; i < kCounterCount; ++i)
      out += ',' + std::to_string(frame.counters[i]);
    out += '\n';
  }
  return out;
}

std::string ThreadsCsv(const Report& report)
{
  std::string out = "thread,os_id,cpu_seconds,utilization";
  for (std::size_t i = 0; i < kZoneCount; ++i)
  {
    out += ',';
    out += ZoneName(static_cast<Zone>(i));
    out += "_ms";
  }
  out += '\n';
  for (const ThreadRecord& thread : report.threads)
  {
    out += CsvField(thread.name);
    out += ',' + std::to_string(thread.id);
    out += ',' + (thread.cpu_seconds < 0.0 ? std::string("unavailable")
                                           : FormatDouble(thread.cpu_seconds, 4));
    out += ',' + (thread.utilization < 0.0 ? std::string("unavailable")
                                           : FormatDouble(thread.utilization, 4));
    for (std::size_t i = 0; i < kZoneCount; ++i)
      out += ',' + FormatDouble(thread.zone_ms[i], 3);
    out += '\n';
  }
  return out;
}

std::string HotspotsCsv(const Report& report)
{
  std::string out = "guest_pc,symbol,samples,percent\n";
  for (const HotspotRecord& hotspot : report.hotspots)
  {
    out += HexAddress(hotspot.guest_pc);
    out += ',' + CsvField(hotspot.symbol.empty() ? "?" : hotspot.symbol);
    out += ',' + std::to_string(hotspot.samples);
    out += ',' + FormatDouble(hotspot.percent, 2);
    out += '\n';
  }
  return out;
}

std::string EventsJsonl(const Report& report)
{
  std::string out;
  for (const Event& event : report.events)
  {
    JsonWriter json(0);
    json.BeginObject();
    json.KeyNumber("time_s", event.time_s);
    json.KeyString("type", EventTypeName(event.type));
    json.KeyNumber("duration_ms", event.duration_ms);
    json.KeyInteger("hash", event.hash);
    json.KeyString("detail", event.detail);
    json.EndObject();
    out += json.Take();
    out += '\n';
  }
  return out;
}

std::string Readme(const Report& report)
{
  std::ostringstream out;
  out << "ModernGekko diagnostics report\n"
      << "==============================\n\n"
      << "Schema version: " << report.schema_version << "\n"
      << "Created (UTC):  " << report.created_utc << "\n"
      << "Capture kind:   " << report.capture_kind << "\n"
      << "Level:          " << LevelName(report.level) << "\n"
      << "Anonymized:     " << (report.anonymized ? "yes" : "no") << "\n\n"
      << "Files\n"
      << "-----\n"
      << "report.json    Index, schema version and the analyzer verdict.\n"
      << "summary.json   Frame statistics, per-zone milliseconds and totals.\n"
      << "system.json    Host CPU, memory, OS and graphics adapter.\n"
      << "build.json     ModernGekko build identity.\n"
      << "game.json      Game and recomp module identity (hashes only).\n"
      << "config.json    Runtime configuration relevant to performance.\n"
      << "frames.csv     Per-frame telemetry.\n"
      << "threads.csv    Per-thread CPU time and zone attribution.\n"
      << "events.jsonl   Timestamped events (shader compilations, stalls).\n"
      << "counters.json  Counter totals for the capture.\n"
      << "hotspots.csv   Sampled guest PC hotspots (trace level only).\n"
      << "runtime.log    Captured runtime log lines.\n\n"
      << "This report contains no game code or data, no screenshots, no save\n"
      << "files and no memory dumps. Games are identified by SHA-256 only.\n";
  return out.str();
}
}  // namespace

std::string AnonymizeText(std::string text)
{
  const std::string home = HomeDirectory();
  if (!home.empty())
  {
    text = ReplaceAll(std::move(text), home, "<USER>");
    std::string generic = home;
    std::replace(generic.begin(), generic.end(), '\\', '/');
    text = ReplaceAll(std::move(text), generic, "<USER>");
  }
  const std::string user = UserName();
  if (user.size() >= 3)
    text = ReplaceAll(std::move(text), user, "<USER>");
  return RedactIpv4(text);
}

std::string AnonymizePath(const std::filesystem::path& path)
{
  std::string generic = path.generic_string();
  return AnonymizeText(std::move(generic));
}

WriteResult WriteReport(const Report& report, const std::filesystem::path& path)
{
  std::vector<detail::ArchiveEntry> entries;

  {
    JsonWriter json;
    json.BeginObject();
    json.KeyInteger("schema_version", static_cast<std::uint64_t>(report.schema_version));
    json.KeyString("generator", "ModernGekko diagnostics");
    json.KeyString("created_utc", report.created_utc);
    json.KeyString("capture_kind", report.capture_kind);
    json.KeyString("level", LevelName(report.level));
    json.KeyBool("anonymized", report.anonymized);
    json.Key("build");
    WriteBuild(json, report.build);
    json.Key("game");
    WriteGame(json, report.game, report.module);
    json.Key("statistics");
    WriteStatistics(json, report.statistics);
    json.Key("analysis");
    WriteAnalysis(json, report.analysis);
    json.EndObject();
    entries.push_back({"report.json", json.Take()});
  }

  {
    JsonWriter json;
    WriteSystem(json, report.system);
    entries.push_back({"system.json", json.Take()});
  }

  {
    JsonWriter json;
    WriteBuild(json, report.build);
    entries.push_back({"build.json", json.Take()});
  }

  {
    JsonWriter json;
    WriteGame(json, report.game, report.module);
    entries.push_back({"game.json", json.Take()});
  }

  {
    JsonWriter json;
    json.BeginObject();
    json.Key("graphics");
    json.BeginObject();
    json.KeyString("backend", report.graphics.backend);
    json.KeyString("adapter", report.graphics.adapter);
    json.KeyString("api_version", report.graphics.api_version);
    json.KeyString("driver", report.graphics.driver);
    json.KeyInteger("internal_resolution_scale",
                    static_cast<std::uint64_t>(std::max(0, report.graphics.internal_resolution_scale)));
    json.KeyString("shader_compilation_mode", report.graphics.shader_compilation_mode);
    json.KeyBool("vsync", report.graphics.vsync);
    json.EndObject();
    json.Key("runtime");
    json.BeginObject();
    for (const auto& [key, value] : report.runtime_config)
      json.KeyString(key, value);
    json.EndObject();
    json.EndObject();
    entries.push_back({"config.json", json.Take()});
  }

  {
    JsonWriter json;
    json.BeginObject();
    json.Key("statistics");
    WriteStatistics(json, report.statistics);
    json.Key("zone_ms_per_frame");
    json.BeginObject();
    for (std::size_t i = 0; i < kZoneCount; ++i)
      json.KeyNumber(ZoneName(static_cast<Zone>(i)), report.zone_ms_per_frame[i]);
    json.EndObject();
    json.Key("analysis");
    WriteAnalysis(json, report.analysis);
    json.EndObject();
    entries.push_back({"summary.json", json.Take()});
  }

  {
    JsonWriter json;
    json.BeginObject();
    for (std::size_t i = 0; i < kCounterCount; ++i)
      json.KeyInteger(CounterName(static_cast<Counter>(i)), report.counters[i]);
    json.EndObject();
    entries.push_back({"counters.json", json.Take()});
  }

  entries.push_back({"frames.csv", FramesCsv(report)});
  entries.push_back({"threads.csv", ThreadsCsv(report)});
  entries.push_back({"events.jsonl", EventsJsonl(report)});
  entries.push_back({"hotspots.csv", HotspotsCsv(report)});

  {
    std::string log;
    for (const std::string& line : report.log_lines)
    {
      log += line;
      log += '\n';
    }
    entries.push_back({"runtime.log", std::move(log)});
  }

  entries.push_back({"README.txt", Readme(report)});

  std::string error;
  if (!detail::WriteArchive(path, entries, &error))
    return WriteResult{false, std::move(error)};
  return WriteResult{true, {}};
}
}  // namespace moderngekko::diagnostics
