#include "diagnostics_internal.hpp"
#include "moderngekko/diagnostics_report.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>

#define PICOJSON_USE_INT64
#include <picojson.h>

namespace moderngekko::diagnostics
{
namespace
{
const picojson::value& Lookup(const picojson::value& node, const char* key)
{
  static const picojson::value kNull;
  if (!node.is<picojson::object>())
    return kNull;
  const auto& object = node.get<picojson::object>();
  const auto it = object.find(key);
  return it == object.end() ? kNull : it->second;
}

std::string AsString(const picojson::value& node)
{
  return node.is<std::string>() ? node.get<std::string>() : std::string{};
}

double AsNumber(const picojson::value& node, double fallback = 0.0)
{
  if (node.is<double>())
    return node.get<double>();
  if (node.is<std::int64_t>())
    return static_cast<double>(node.get<std::int64_t>());
  return fallback;
}

std::uint64_t AsInteger(const picojson::value& node)
{
  if (node.is<std::int64_t>())
  {
    const std::int64_t value = node.get<std::int64_t>();
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
  }
  if (node.is<double>())
    return static_cast<std::uint64_t>(std::max(0.0, node.get<double>()));
  return 0;
}

bool AsBool(const picojson::value& node)
{
  return node.is<bool>() && node.get<bool>();
}

// Null in the document means "not enough samples", which the model stores as -1.
double AsOptionalNumber(const picojson::value& node)
{
  return node.is<picojson::null>() ? -1.0 : AsNumber(node, -1.0);
}

double ParseDouble(std::string_view text)
{
  double value = 0.0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? value : 0.0;
}

std::uint64_t ParseInteger(std::string_view text)
{
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? value : 0;
}

std::vector<std::string> SplitCsvLine(std::string_view line)
{
  std::vector<std::string> fields;
  std::string current;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i)
  {
    const char c = line[i];
    if (quoted)
    {
      if (c == '"')
      {
        if (i + 1 < line.size() && line[i + 1] == '"')
        {
          current += '"';
          ++i;
        }
        else
        {
          quoted = false;
        }
      }
      else
      {
        current += c;
      }
      continue;
    }
    if (c == '"')
      quoted = true;
    else if (c == ',')
      fields.push_back(std::exchange(current, std::string{}));
    else
      current += c;
  }
  fields.push_back(std::move(current));
  return fields;
}

std::vector<std::string> SplitLines(const std::string& text)
{
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
  }
  return lines;
}

std::uint32_t ParseAddress(std::string_view text)
{
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    text.remove_prefix(2);
  std::uint32_t value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return value;
}

Classification ParseClassification(std::string_view text)
{
  const Classification all[] = {
      Classification::Unknown,
      Classification::GuestCpuBound,
      Classification::StaticRecompSlowPathBound,
      Classification::GpuBound,
      Classification::CpuGpuSynchronizationBound,
      Classification::RendererCpuBound,
      Classification::ShaderCompilationHitching,
      Classification::TextureOrEfbTransferBound,
      Classification::AudioOrTimingWait,
      Classification::NetplayWait,
      Classification::ThreadContention,
      Classification::PossibleHardwareThrottling,
      Classification::InsufficientEvidence,
  };
  for (const Classification value : all)
  {
    if (text == ClassificationName(value))
      return value;
  }
  return Classification::Unknown;
}

Confidence ParseConfidence(std::string_view text)
{
  if (text == "HIGH")
    return Confidence::High;
  if (text == "MEDIUM")
    return Confidence::Medium;
  return Confidence::Low;
}

EventType ParseEventType(std::string_view text)
{
  const EventType all[] = {EventType::CaptureStart,       EventType::CaptureStop,
                           EventType::ShaderCompilation,  EventType::PipelineCompilation,
                           EventType::ShaderCacheMiss,    EventType::GpuStall,
                           EventType::LongFrame,          EventType::Note};
  for (const EventType value : all)
  {
    if (text == EventTypeName(value))
      return value;
  }
  return EventType::Note;
}

FrameStatistics ReadStatistics(const picojson::value& node)
{
  FrameStatistics stats;
  stats.frame_count = static_cast<std::size_t>(AsInteger(Lookup(node, "frame_count")));
  stats.duration_s = AsNumber(Lookup(node, "duration_s"));
  stats.average_fps = AsNumber(Lookup(node, "average_fps"));
  stats.average_frame_ms = AsNumber(Lookup(node, "average_frame_ms"));
  stats.median_frame_ms = AsNumber(Lookup(node, "median_frame_ms"));
  stats.p90_frame_ms = AsNumber(Lookup(node, "p90_frame_ms"));
  stats.p95_frame_ms = AsNumber(Lookup(node, "p95_frame_ms"));
  stats.p99_frame_ms = AsNumber(Lookup(node, "p99_frame_ms"));
  stats.p999_frame_ms = AsOptionalNumber(Lookup(node, "p999_frame_ms"));
  stats.max_frame_ms = AsNumber(Lookup(node, "max_frame_ms"));
  stats.low_1_percent_fps = AsNumber(Lookup(node, "low_1_percent_fps"));
  stats.low_01_percent_fps = AsOptionalNumber(Lookup(node, "low_0_1_percent_fps"));
  stats.over_budget_percent = AsNumber(Lookup(node, "over_budget_percent"));
  stats.target_frame_ms = AsNumber(Lookup(node, "target_frame_ms"));
  stats.average_speed = AsNumber(Lookup(node, "average_speed"));
  stats.min_speed = AsNumber(Lookup(node, "min_speed"));
  stats.average_vps = AsNumber(Lookup(node, "average_vps"));
  return stats;
}

Analysis ReadAnalysis(const picojson::value& node)
{
  Analysis analysis;
  analysis.headline = AsString(Lookup(node, "headline"));
  const picojson::value& findings = Lookup(node, "findings");
  if (!findings.is<picojson::array>())
    return analysis;
  for (const picojson::value& entry : findings.get<picojson::array>())
  {
    Finding finding;
    finding.classification = ParseClassification(AsString(Lookup(entry, "classification")));
    finding.confidence = ParseConfidence(AsString(Lookup(entry, "confidence")));
    const picojson::value& evidence = Lookup(entry, "evidence");
    if (evidence.is<picojson::array>())
    {
      for (const picojson::value& line : evidence.get<picojson::array>())
        finding.evidence.push_back(AsString(line));
    }
    analysis.findings.push_back(std::move(finding));
  }
  return analysis;
}

BuildInfo ReadBuild(const picojson::value& node)
{
  BuildInfo build;
  build.version = AsString(Lookup(node, "version"));
  build.git_commit = AsString(Lookup(node, "git_commit"));
  build.git_state = AsString(Lookup(node, "git_state"));
  build.configuration = AsString(Lookup(node, "configuration"));
  build.compiler = AsString(Lookup(node, "compiler"));
  build.compiler_version = AsString(Lookup(node, "compiler_version"));
  build.host_architecture = AsString(Lookup(node, "host_architecture"));
  build.cpu_backend = AsString(Lookup(node, "cpu_backend"));
  build.link_time_optimization = AsBool(Lookup(node, "link_time_optimization"));
  build.build_timestamp = AsString(Lookup(node, "build_timestamp"));
  return build;
}

void ReadGame(const picojson::value& node, GameIdentity* game, ModuleIdentity* module)
{
  game->title = AsString(Lookup(node, "title"));
  game->disc_id = AsString(Lookup(node, "disc_id"));
  game->platform = AsString(Lookup(node, "platform"));
  game->dol_sha256 = AsString(Lookup(node, "dol_sha256"));
  game->rel_sha256 = AsString(Lookup(node, "rel_sha256"));
  game->assets_sha256 = AsString(Lookup(node, "assets_sha256"));
  game->entry_point = ParseAddress(AsString(Lookup(node, "entry_point")));
  const picojson::value& recomp = Lookup(node, "recomp_module");
  module->kind = AsString(Lookup(recomp, "kind"));
  module->file_name = AsString(Lookup(recomp, "file_name"));
  module->sha256 = AsString(Lookup(recomp, "sha256"));
}

SystemInfo ReadSystem(const picojson::value& node)
{
  SystemInfo system;
  const picojson::value& cpu = Lookup(node, "cpu");
  system.cpu_model = AsString(Lookup(cpu, "model"));
  system.physical_cores = static_cast<int>(AsInteger(Lookup(cpu, "physical_cores")));
  system.logical_processors = static_cast<int>(AsInteger(Lookup(cpu, "logical_processors")));
  system.architecture = AsString(Lookup(cpu, "architecture"));
  const picojson::value& sets = Lookup(cpu, "instruction_sets");
  if (sets.is<picojson::array>())
  {
    for (const picojson::value& entry : sets.get<picojson::array>())
      system.instruction_sets.push_back(AsString(entry));
  }
  const picojson::value& memory = Lookup(node, "memory");
  system.total_physical_memory_bytes = AsInteger(Lookup(memory, "total_physical_bytes"));
  system.process_resident_bytes = AsInteger(Lookup(memory, "process_resident_bytes"));
  system.process_peak_bytes = AsInteger(Lookup(memory, "process_peak_bytes"));
  const picojson::value& os = Lookup(node, "os");
  system.os_name = AsString(Lookup(os, "name"));
  system.os_version = AsString(Lookup(os, "version"));
  system.os_architecture = AsString(Lookup(os, "architecture"));
  return system;
}

bool ParseJson(const std::string& text, picojson::value* out, std::string* error)
{
  const std::string parse_error = picojson::parse(*out, text);
  if (!parse_error.empty())
  {
    if (error != nullptr)
      *error = parse_error;
    return false;
  }
  return true;
}
}  // namespace

ReadResult ReadReport(const std::filesystem::path& path)
{
  ReadResult result;
  std::vector<detail::ArchiveEntry> entries;
  if (!detail::ReadArchive(path, &entries, &result.error))
    return result;

  std::map<std::string, std::string> files;
  for (detail::ArchiveEntry& entry : entries)
    files.emplace(entry.name, std::move(entry.data));

  const auto file = [&](const char* name) -> const std::string* {
    const auto it = files.find(name);
    return it == files.end() ? nullptr : &it->second;
  };

  const std::string* index = file("report.json");
  if (index == nullptr)
  {
    result.error = path.string() + " is missing report.json";
    return result;
  }
  picojson::value root;
  if (!ParseJson(*index, &root, &result.error))
    return result;

  Report& report = result.report;
  report.schema_version = static_cast<int>(AsInteger(Lookup(root, "schema_version")));
  if (report.schema_version <= 0)
  {
    result.error = path.string() + " has no usable schema_version";
    return result;
  }
  if (report.schema_version > kSchemaVersion)
  {
    result.error = "report schema version " + std::to_string(report.schema_version) +
                   " is newer than this build understands (" + std::to_string(kSchemaVersion) + ")";
    return result;
  }
  report.created_utc = AsString(Lookup(root, "created_utc"));
  report.capture_kind = AsString(Lookup(root, "capture_kind"));
  ParseLevel(AsString(Lookup(root, "level")), &report.level);
  report.anonymized = AsBool(Lookup(root, "anonymized"));
  report.build = ReadBuild(Lookup(root, "build"));
  ReadGame(Lookup(root, "game"), &report.game, &report.module);
  report.statistics = ReadStatistics(Lookup(root, "statistics"));
  report.analysis = ReadAnalysis(Lookup(root, "analysis"));

  if (const std::string* text = file("system.json"); text != nullptr)
  {
    picojson::value node;
    if (ParseJson(*text, &node, nullptr))
      report.system = ReadSystem(node);
  }

  if (const std::string* text = file("summary.json"); text != nullptr)
  {
    picojson::value node;
    if (ParseJson(*text, &node, nullptr))
    {
      const picojson::value& zones = Lookup(node, "zone_ms_per_frame");
      for (std::size_t i = 0; i < kZoneCount; ++i)
        report.zone_ms_per_frame[i] = AsNumber(Lookup(zones, ZoneName(static_cast<Zone>(i))));
    }
  }

  if (const std::string* text = file("counters.json"); text != nullptr)
  {
    picojson::value node;
    if (ParseJson(*text, &node, nullptr))
    {
      for (std::size_t i = 0; i < kCounterCount; ++i)
        report.counters[i] = AsInteger(Lookup(node, CounterName(static_cast<Counter>(i))));
    }
  }

  if (const std::string* text = file("config.json"); text != nullptr)
  {
    picojson::value node;
    if (ParseJson(*text, &node, nullptr))
    {
      const picojson::value& graphics = Lookup(node, "graphics");
      report.graphics.backend = AsString(Lookup(graphics, "backend"));
      report.graphics.adapter = AsString(Lookup(graphics, "adapter"));
      report.graphics.api_version = AsString(Lookup(graphics, "api_version"));
      report.graphics.driver = AsString(Lookup(graphics, "driver"));
      report.graphics.internal_resolution_scale =
          static_cast<int>(AsInteger(Lookup(graphics, "internal_resolution_scale")));
      report.graphics.shader_compilation_mode =
          AsString(Lookup(graphics, "shader_compilation_mode"));
      report.graphics.vsync = AsBool(Lookup(graphics, "vsync"));
      const picojson::value& runtime = Lookup(node, "runtime");
      if (runtime.is<picojson::object>())
      {
        for (const auto& [key, value] : runtime.get<picojson::object>())
          report.runtime_config.emplace(key, AsString(value));
      }
    }
  }

  if (const std::string* text = file("frames.csv"); text != nullptr)
  {
    const std::vector<std::string> lines = SplitLines(*text);
    constexpr std::size_t kFixedColumns = 5;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
      if (lines[i].empty())
        continue;
      const std::vector<std::string> fields = SplitCsvLine(lines[i]);
      if (fields.size() < kFixedColumns + kZoneCount + kCounterCount)
        continue;
      FrameRecord frame;
      frame.time_s = ParseDouble(fields[0]);
      frame.frame_ms = ParseDouble(fields[1]);
      frame.fps = ParseDouble(fields[2]);
      frame.vps = ParseDouble(fields[3]);
      frame.speed = ParseDouble(fields[4]);
      for (std::size_t z = 0; z < kZoneCount; ++z)
        frame.zone_ms[z] = static_cast<float>(ParseDouble(fields[kFixedColumns + z]));
      for (std::size_t c = 0; c < kCounterCount; ++c)
      {
        frame.counters[c] =
            static_cast<std::uint32_t>(ParseInteger(fields[kFixedColumns + kZoneCount + c]));
      }
      report.frames.push_back(frame);
    }
  }

  if (const std::string* text = file("threads.csv"); text != nullptr)
  {
    const std::vector<std::string> lines = SplitLines(*text);
    constexpr std::size_t kFixedColumns = 4;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
      if (lines[i].empty())
        continue;
      const std::vector<std::string> fields = SplitCsvLine(lines[i]);
      if (fields.size() < kFixedColumns + kZoneCount)
        continue;
      ThreadRecord thread;
      thread.name = fields[0];
      thread.id = ParseInteger(fields[1]);
      thread.cpu_seconds = fields[2] == "unavailable" ? -1.0 : ParseDouble(fields[2]);
      thread.utilization = fields[3] == "unavailable" ? -1.0 : ParseDouble(fields[3]);
      for (std::size_t z = 0; z < kZoneCount; ++z)
        thread.zone_ms[z] = ParseDouble(fields[kFixedColumns + z]);
      report.threads.push_back(std::move(thread));
    }
  }

  if (const std::string* text = file("hotspots.csv"); text != nullptr)
  {
    const std::vector<std::string> lines = SplitLines(*text);
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
      if (lines[i].empty())
        continue;
      const std::vector<std::string> fields = SplitCsvLine(lines[i]);
      if (fields.size() < 4)
        continue;
      HotspotRecord hotspot;
      hotspot.guest_pc = ParseAddress(fields[0]);
      hotspot.symbol = fields[1];
      hotspot.samples = ParseInteger(fields[2]);
      hotspot.percent = ParseDouble(fields[3]);
      report.hotspots.push_back(std::move(hotspot));
    }
  }

  if (const std::string* text = file("events.jsonl"); text != nullptr)
  {
    for (const std::string& line : SplitLines(*text))
    {
      if (line.empty())
        continue;
      picojson::value node;
      if (!ParseJson(line, &node, nullptr))
        continue;
      Event event;
      event.time_s = AsNumber(Lookup(node, "time_s"));
      event.type = ParseEventType(AsString(Lookup(node, "type")));
      event.duration_ms = AsNumber(Lookup(node, "duration_ms"));
      event.hash = AsInteger(Lookup(node, "hash"));
      event.detail = AsString(Lookup(node, "detail"));
      report.events.push_back(std::move(event));
    }
  }

  if (const std::string* text = file("runtime.log"); text != nullptr)
    report.log_lines = SplitLines(*text);

  result.ok = true;
  return result;
}
}  // namespace moderngekko::diagnostics
