#include "diagnostics_internal.hpp"
#include "moderngekko/diagnostics_report.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <unordered_map>

#define PICOJSON_USE_INT64
#include <picojson.h>

namespace moderngekko::diagnostics
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxLogLines = 2000;
constexpr std::size_t kMaxEvents = 4096;
constexpr std::size_t kMaxHotspots = 512;
// Frame history is bounded by an assumed worst-case presentation rate so the
// ring never grows even if a host presents far above 60 Hz.
constexpr double kAssumedMaxFps = 250.0;

std::size_t FrameCapacity(double seconds, std::size_t lower, std::size_t upper)
{
  const double frames = std::ceil(std::max(seconds, 0.0) * kAssumedMaxFps);
  const auto clamped = static_cast<std::size_t>(std::clamp(frames, 0.0, 1.0e7));
  return std::clamp(clamped, lower, upper);
}

std::string SanitizeFileComponent(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  for (const char c : text)
  {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
    out += safe ? c : '-';
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out;
}

std::string FileTimestamp()
{
  std::string stamp = detail::UtcTimestamp();
  std::string out;
  for (const char c : stamp)
  {
    if (c == ':' || c == '-')
      continue;
    out += c;
  }
  return out;
}
}  // namespace

struct Diagnostics::Impl
{
  mutable std::mutex mutex;
  Config config;
  bool initialized = false;
  bool capturing = false;
  Clock::time_point epoch;
  Clock::time_point capture_start;
  Clock::time_point last_frame;

  GameIdentity game;
  ModuleIdentity module;
  GraphicsIdentity graphics;
  std::map<std::uint32_t, std::string> symbols;

  detail::TraceBuffer history;
  detail::TraceBuffer capture;
  std::vector<std::string> log_lines;

  // Aggregate snapshots used to turn running totals into per-frame deltas.
  std::array<std::uint64_t, kZoneCount> last_zone_ns{};
  std::array<std::uint64_t, kCounterCount> last_counters{};

  struct ThreadBaseline
  {
    std::array<std::uint64_t, kZoneCount> zone_ns{};
    double cpu_seconds = -1.0;
  };
  std::map<detail::ThreadState*, ThreadBaseline> capture_baselines;

  std::jthread sampler;
  std::mutex hotspot_mutex;
  std::unordered_map<std::uint32_t, std::uint64_t> hotspots;
  std::uint64_t hotspot_samples = 0;

  void AggregateTotals(std::array<std::uint64_t, kZoneCount>* zones,
                       std::array<std::uint64_t, kCounterCount>* counters) const
  {
    zones->fill(0);
    counters->fill(0);
    for (detail::ThreadState* state : detail::RegisteredThreads())
    {
      for (std::size_t i = 0; i < kZoneCount; ++i)
        (*zones)[i] += state->zone_ns[i].load(std::memory_order_relaxed);
      for (std::size_t i = 0; i < kCounterCount; ++i)
        (*counters)[i] += state->counters[i].load(std::memory_order_relaxed);
    }
  }

  void SamplerLoop(std::stop_token stop_token, unsigned hz)
  {
    const auto period = std::chrono::nanoseconds(1000000000ull / std::max(1u, hz));
    auto next = Clock::now();
    while (!stop_token.stop_requested())
    {
      next += period;
      std::this_thread::sleep_until(next);
      if (stop_token.stop_requested())
        break;
      bool active = false;
      {
        std::lock_guard lock(mutex);
        active = capturing;
      }
      if (!active)
        continue;
      for (detail::ThreadState* state : detail::RegisteredThreads())
      {
        if (state->guest_pc_epoch.load(std::memory_order_relaxed) == 0)
          continue;
        const std::uint32_t pc = state->guest_pc.load(std::memory_order_relaxed);
        std::lock_guard lock(hotspot_mutex);
        ++hotspot_samples;
        auto it = hotspots.find(pc);
        if (it != hotspots.end())
          ++it->second;
        else if (hotspots.size() < 65536)
          hotspots.emplace(pc, 1);
      }
    }
  }
};

Diagnostics::Diagnostics() : m_impl(std::make_unique<Impl>()) {}

Diagnostics::~Diagnostics()
{
  Shutdown();
}

Diagnostics& Diagnostics::Get()
{
  static Diagnostics instance;
  return instance;
}

void Diagnostics::Initialize(Config config)
{
  Shutdown();
  std::lock_guard lock(m_impl->mutex);
  if (config.output_directory.empty())
    config.output_directory = std::filesystem::current_path() / "diagnostics";
  config.sample_hz = std::clamp<unsigned>(config.sample_hz, 50u, 2000u);
  config.history_seconds = std::clamp(config.history_seconds, 1.0, 600.0);
  if (config.target_frame_ms <= 0.0)
    config.target_frame_ms = 16.7;
  m_impl->config = std::move(config);
  m_impl->initialized = true;
  m_impl->epoch = Clock::now();
  m_impl->last_frame = m_impl->epoch;
  m_impl->log_lines.clear();
  m_impl->history.Reset(FrameCapacity(m_impl->config.history_seconds, 64, 30000), 512);
  m_impl->AggregateTotals(&m_impl->last_zone_ns, &m_impl->last_counters);

  if (!m_impl->config.enabled)
  {
    detail::g_level.store(0, std::memory_order_relaxed);
    return;
  }
  detail::g_level.store(static_cast<std::uint32_t>(m_impl->config.level),
                        std::memory_order_relaxed);
  if (m_impl->config.level == Level::Trace)
  {
    const unsigned hz = m_impl->config.sample_hz;
    m_impl->sampler = std::jthread(
        [this, hz](std::stop_token token) { m_impl->SamplerLoop(std::move(token), hz); });
  }
}

void Diagnostics::Shutdown()
{
  detail::g_level.store(0, std::memory_order_relaxed);
  if (m_impl->sampler.joinable())
  {
    m_impl->sampler.request_stop();
    m_impl->sampler.join();
  }
  std::lock_guard lock(m_impl->mutex);
  m_impl->capturing = false;
  m_impl->initialized = false;
  m_impl->capture_baselines.clear();
  m_impl->history.Clear();
  m_impl->capture.Clear();
  {
    std::lock_guard hotspot_lock(m_impl->hotspot_mutex);
    m_impl->hotspots.clear();
    m_impl->hotspot_samples = 0;
  }
}

bool Diagnostics::IsEnabled() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->initialized && m_impl->config.enabled;
}

bool Diagnostics::IsCapturing() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->capturing;
}

Level Diagnostics::GetLevel() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->config.enabled ? m_impl->config.level : Level::Off;
}

const Config& Diagnostics::GetConfig() const
{
  return m_impl->config;
}

void Diagnostics::SetGameIdentity(GameIdentity identity)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->game = std::move(identity);
}

void Diagnostics::SetModuleIdentity(ModuleIdentity identity)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->module = std::move(identity);
}

void Diagnostics::SetGraphicsIdentity(GraphicsIdentity identity)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->graphics = std::move(identity);
}

void Diagnostics::SetGuestSymbols(std::vector<std::pair<std::uint32_t, std::string>> symbols)
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->symbols.clear();
  for (auto& [address, name] : symbols)
    m_impl->symbols.emplace(address, std::move(name));
}

bool Diagnostics::LoadGuestSymbols(const std::filesystem::path& path, std::string* error)
{
  std::ifstream file(path);
  if (!file)
  {
    if (error != nullptr)
      *error = "could not open " + path.string();
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  picojson::value root;
  const std::string parse_error = picojson::parse(root, buffer.str());
  if (!parse_error.empty())
  {
    if (error != nullptr)
      *error = parse_error;
    return false;
  }
  if (!root.is<picojson::object>())
  {
    if (error != nullptr)
      *error = path.string() + " is not a symbol object";
    return false;
  }
  std::vector<std::pair<std::uint32_t, std::string>> symbols;
  for (const auto& [key, value] : root.get<picojson::object>())
  {
    std::string_view text = key;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
      text.remove_prefix(2);
    std::uint32_t address = 0;
    if (std::from_chars(text.data(), text.data() + text.size(), address, 16).ec != std::errc{})
      continue;
    std::string name;
    if (value.is<std::string>())
    {
      name = value.get<std::string>();
    }
    else if (value.is<picojson::object>())
    {
      const auto& object = value.get<picojson::object>();
      for (const char* field : {"guest_symbol", "symbol", "generated_symbol"})
      {
        const auto it = object.find(field);
        if (it != object.end() && it->second.is<std::string>())
        {
          name = it->second.get<std::string>();
          break;
        }
      }
    }
    if (!name.empty())
      symbols.emplace_back(address, std::move(name));
  }
  SetGuestSymbols(std::move(symbols));
  return true;
}

void Diagnostics::NameCurrentThread(std::string name)
{
  if (!Active())
    return;
  detail::SetThreadName(detail::Tls(), std::move(name));
}

void Diagnostics::SampleCurrentThreadCpu()
{
  if (!Active())
    return;
  const double seconds = detail::CurrentThreadCpuSeconds();
  if (seconds >= 0.0)
    detail::Tls().cpu_seconds.store(seconds, std::memory_order_relaxed);
}

void Diagnostics::AppendLog(std::string line)
{
  std::lock_guard lock(m_impl->mutex);
  if (!m_impl->initialized)
    return;
  if (m_impl->log_lines.size() >= kMaxLogLines)
    m_impl->log_lines.erase(m_impl->log_lines.begin());
  m_impl->log_lines.push_back(std::move(line));
}

bool Diagnostics::StartCapture()
{
  if (!IsEnabled())
    return false;
  std::lock_guard lock(m_impl->mutex);
  if (m_impl->capturing)
    return false;
  const double seconds =
      m_impl->config.capture_seconds > 0.0 ? m_impl->config.capture_seconds : 180.0;
  m_impl->capture.Reset(FrameCapacity(seconds, 256, 60000), kMaxEvents);
  m_impl->capture_start = Clock::now();
  m_impl->capture_baselines.clear();
  for (detail::ThreadState* state : detail::RegisteredThreads())
  {
    Impl::ThreadBaseline baseline;
    for (std::size_t i = 0; i < kZoneCount; ++i)
      baseline.zone_ns[i] = state->zone_ns[i].load(std::memory_order_relaxed);
    baseline.cpu_seconds = state->cpu_seconds.load(std::memory_order_relaxed);
    m_impl->capture_baselines.emplace(state, baseline);
  }
  {
    std::lock_guard hotspot_lock(m_impl->hotspot_mutex);
    m_impl->hotspots.clear();
    m_impl->hotspot_samples = 0;
  }
  m_impl->capturing = true;
  Event event;
  event.time_s = std::chrono::duration<double>(m_impl->capture_start - m_impl->epoch).count();
  event.type = EventType::CaptureStart;
  event.detail = "capture started";
  m_impl->capture.PushEvent(event);
  m_impl->history.PushEvent(event);
  return true;
}

void Diagnostics::ToggleCapture()
{
  if (IsCapturing())
    StopCapture();
  else
    StartCapture();
}

void Diagnostics::RecordEvent(EventType type, double duration_ms, std::uint64_t hash,
                              std::string detail_text)
{
  if (!Active())
    return;
  Event event;
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->initialized)
      return;
    event.time_s = std::chrono::duration<double>(Clock::now() - m_impl->epoch).count();
  }
  event.type = type;
  event.duration_ms = duration_ms;
  event.hash = hash;
  event.detail = std::move(detail_text);
  m_impl->history.PushEvent(event);
  if (IsCapturing())
    m_impl->capture.PushEvent(event);
}

void Diagnostics::EndFrame(const FrameTelemetry& telemetry)
{
  if (!Active())
    return;
  SampleCurrentThreadCpu();

  FrameRecord frame;
  bool capture_expired = false;
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->initialized)
      return;
    const auto now = Clock::now();
    frame.time_s = std::chrono::duration<double>(now - m_impl->epoch).count();
    frame.frame_ms = std::chrono::duration<double, std::milli>(now - m_impl->last_frame).count();
    m_impl->last_frame = now;
    frame.fps = telemetry.fps;
    frame.vps = telemetry.vps;
    frame.speed = telemetry.speed;

    std::array<std::uint64_t, kZoneCount> zones{};
    std::array<std::uint64_t, kCounterCount> counters{};
    m_impl->AggregateTotals(&zones, &counters);
    for (std::size_t i = 0; i < kZoneCount; ++i)
    {
      const std::uint64_t delta =
          zones[i] >= m_impl->last_zone_ns[i] ? zones[i] - m_impl->last_zone_ns[i] : 0;
      frame.zone_ms[i] = static_cast<float>(static_cast<double>(delta) / 1.0e6);
      m_impl->last_zone_ns[i] = zones[i];
    }
    for (std::size_t i = 0; i < kCounterCount; ++i)
    {
      const std::uint64_t delta =
          counters[i] >= m_impl->last_counters[i] ? counters[i] - m_impl->last_counters[i] : 0;
      frame.counters[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(delta, 0xFFFFFFFFull));
      m_impl->last_counters[i] = counters[i];
    }

    if (m_impl->capturing && m_impl->config.capture_seconds > 0.0)
    {
      const double elapsed =
          std::chrono::duration<double>(now - m_impl->capture_start).count();
      capture_expired = elapsed >= m_impl->config.capture_seconds;
    }
  }

  m_impl->history.PushFrame(frame);
  if (IsCapturing())
  {
    m_impl->capture.PushFrame(frame);
    // A frame far above the running budget is worth an explicit marker so the
    // analyzer can line it up with shader compilations.
    const double budget = m_impl->config.target_frame_ms;
    if (budget > 0.0 && frame.frame_ms > 3.0 * budget)
    {
      Event event;
      event.time_s = frame.time_s;
      event.type = EventType::LongFrame;
      event.duration_ms = frame.frame_ms;
      event.detail = "frame exceeded three times the target budget";
      m_impl->capture.PushEvent(event);
    }
  }

  if (capture_expired)
    StopCapture();
}

std::vector<FrameRecord> Diagnostics::RecentFrames(std::size_t count) const
{
  return m_impl->history.RecentFrames(count);
}

CaptureResult Diagnostics::StopCapture()
{
  CaptureResult result;
  std::vector<FrameRecord> frames;
  std::vector<Event> events;
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->capturing)
    {
      result.error = "no capture is running";
      return result;
    }
    m_impl->capturing = false;
  }
  Event stop_event;
  stop_event.time_s = std::chrono::duration<double>(Clock::now() - m_impl->epoch).count();
  stop_event.type = EventType::CaptureStop;
  stop_event.detail = "capture stopped";
  m_impl->capture.PushEvent(stop_event);
  m_impl->history.PushEvent(stop_event);

  frames = m_impl->capture.Frames();
  events = m_impl->capture.Events();
  return WriteCapture(frames, events, "capture");
}

CaptureResult Diagnostics::WriteCapture(const std::vector<FrameRecord>& frames,
                                        const std::vector<Event>& events,
                                        std::string capture_kind)
{
  CaptureResult result;
  Report report;
  report.schema_version = kSchemaVersion;
  report.created_utc = detail::UtcTimestamp();
  report.capture_kind = std::move(capture_kind);
  report.build = CurrentBuildInfo();
  report.system = CollectSystemInfo();
  report.frames = frames;
  report.events = events;

  Config config;
  std::map<detail::ThreadState*, Impl::ThreadBaseline> baselines;
  std::map<std::uint32_t, std::string> symbols;
  {
    std::lock_guard lock(m_impl->mutex);
    config = m_impl->config;
    report.level = m_impl->config.level;
    report.anonymized = m_impl->config.anonymize;
    report.game = m_impl->game;
    report.module = m_impl->module;
    report.graphics = m_impl->graphics;
    report.log_lines = m_impl->log_lines;
    baselines = m_impl->capture_baselines;
    symbols = m_impl->symbols;
  }

  report.runtime_config.emplace("diagnostics_level", LevelName(report.level));
  report.runtime_config.emplace("diagnostics_overlay", config.overlay ? "true" : "false");
  report.runtime_config.emplace("target_frame_ms", detail::FormatDouble(config.target_frame_ms, 3));
  report.runtime_config.emplace("history_seconds", detail::FormatDouble(config.history_seconds, 1));
  report.runtime_config.emplace("guest_pc_sample_hz", std::to_string(config.sample_hz));

  report.statistics = ComputeFrameStatistics(report.frames, config.target_frame_ms);

  const double frame_count = static_cast<double>(std::max<std::size_t>(report.frames.size(), 1));
  for (const FrameRecord& frame : report.frames)
  {
    for (std::size_t i = 0; i < kZoneCount; ++i)
      report.zone_ms_per_frame[i] += frame.zone_ms[i];
    for (std::size_t i = 0; i < kCounterCount; ++i)
      report.counters[i] += frame.counters[i];
  }
  for (std::size_t i = 0; i < kZoneCount; ++i)
    report.zone_ms_per_frame[i] /= frame_count;

  const double capture_seconds = report.statistics.duration_s;
  for (detail::ThreadState* state : detail::RegisteredThreads())
  {
    ThreadRecord record;
    record.name = detail::ThreadName(*state);
    if (record.name.empty())
      record.name = "thread-" + std::to_string(state->os_thread_id);
    record.id = state->os_thread_id;
    const auto baseline = baselines.find(state);
    const double now_cpu = state->cpu_seconds.load(std::memory_order_relaxed);
    const double then_cpu =
        baseline == baselines.end() ? -1.0 : baseline->second.cpu_seconds;
    if (now_cpu >= 0.0)
      record.cpu_seconds = then_cpu >= 0.0 ? now_cpu - then_cpu : now_cpu;
    if (record.cpu_seconds >= 0.0 && capture_seconds > 0.0)
      record.utilization = record.cpu_seconds / capture_seconds;
    bool any_zone = false;
    for (std::size_t i = 0; i < kZoneCount; ++i)
    {
      const std::uint64_t total = state->zone_ns[i].load(std::memory_order_relaxed);
      const std::uint64_t base = baseline == baselines.end() ? 0 : baseline->second.zone_ns[i];
      const std::uint64_t delta = total >= base ? total - base : total;
      record.zone_ms[i] = static_cast<double>(delta) / 1.0e6;
      any_zone = any_zone || delta != 0;
    }
    if (!any_zone && record.cpu_seconds < 0.0)
      continue;
    report.threads.push_back(std::move(record));
  }

  {
    std::lock_guard lock(m_impl->hotspot_mutex);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> sorted(m_impl->hotspots.begin(),
                                                                m_impl->hotspots.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    const double total = static_cast<double>(std::max<std::uint64_t>(m_impl->hotspot_samples, 1));
    const std::size_t shown = std::min(sorted.size(), kMaxHotspots);
    for (std::size_t i = 0; i < shown; ++i)
    {
      HotspotRecord hotspot;
      hotspot.guest_pc = sorted[i].first;
      hotspot.samples = sorted[i].second;
      hotspot.percent = 100.0 * static_cast<double>(sorted[i].second) / total;
      const auto symbol = symbols.find(sorted[i].first);
      if (symbol != symbols.end())
        hotspot.symbol = symbol->second;
      report.hotspots.push_back(std::move(hotspot));
    }
  }

  AnalysisInput input;
  input.frames = report.statistics;
  input.zone_ms_per_frame = report.zone_ms_per_frame;
  input.counters = report.counters;
  input.events = report.events;
  input.threads = report.threads;
  input.level = report.level;
  report.analysis = Analyze(input);

  if (report.anonymized)
  {
    report.game.title = AnonymizeText(report.game.title);
    report.module.file_name = AnonymizeText(report.module.file_name);
    report.graphics.adapter = AnonymizeText(report.graphics.adapter);
    report.graphics.driver = AnonymizeText(report.graphics.driver);
    for (std::string& line : report.log_lines)
      line = AnonymizeText(std::move(line));
    for (Event& event : report.events)
      event.detail = AnonymizeText(std::move(event.detail));
    for (ThreadRecord& thread : report.threads)
      thread.name = AnonymizeText(std::move(thread.name));
  }

  std::error_code ec;
  std::filesystem::create_directories(config.output_directory, ec);
  const std::string disc =
      report.game.disc_id.empty() ? std::string("unknown") : SanitizeFileComponent(report.game.disc_id);
  const std::filesystem::path path =
      config.output_directory /
      ("moderngekko-" + disc + '-' + report.capture_kind + '-' + FileTimestamp() + ".mgdiag");

  const WriteResult written = WriteReport(report, path);
  result.ok = written.ok;
  result.error = written.error;
  result.report_path = path;
  result.frame_count = report.frames.size();
  result.duration_s = report.statistics.duration_s;
  return result;
}

CaptureResult Diagnostics::SaveHistory()
{
  if (!IsEnabled())
  {
    CaptureResult result;
    result.error = "diagnostics are not enabled";
    return result;
  }
  return WriteCapture(m_impl->history.Frames(), m_impl->history.Events(), "history");
}
}  // namespace moderngekko::diagnostics
