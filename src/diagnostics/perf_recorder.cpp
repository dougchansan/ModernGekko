#include "diagnostics_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

namespace moderngekko::diagnostics
{
namespace detail
{
namespace
{
// Bounded so a runtime that churns threads cannot grow the registry without
// limit; extra threads still execute, they just share the overflow state.
constexpr std::size_t kMaxThreadStates = 128;

struct Registry
{
  std::mutex mutex;
  std::vector<std::unique_ptr<ThreadState>> states;
  std::map<const ThreadState*, std::string> names;
  ThreadState* overflow = nullptr;
};

Registry& GetRegistry()
{
  static Registry registry;
  return registry;
}
}  // namespace

std::atomic<std::uint32_t> g_level{0};
thread_local ThreadState* t_state = nullptr;

ThreadState& AcquireThreadState()
{
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  if (registry.states.size() >= kMaxThreadStates)
  {
    if (registry.overflow == nullptr)
      registry.overflow = registry.states.front().get();
    t_state = registry.overflow;
    return *t_state;
  }
  auto state = std::make_unique<ThreadState>();
  state->os_thread_id = CurrentOsThreadId();
  ThreadState* raw = state.get();
  registry.states.push_back(std::move(state));
  t_state = raw;
  return *raw;
}

std::vector<ThreadState*> RegisteredThreads()
{
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  std::vector<ThreadState*> out;
  out.reserve(registry.states.size());
  for (const auto& state : registry.states)
    out.push_back(state.get());
  return out;
}

void SetThreadName(ThreadState& state, std::string name)
{
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  registry.names[&state] = std::move(name);
}

std::string ThreadName(const ThreadState& state)
{
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  const auto it = registry.names.find(&state);
  return it == registry.names.end() ? std::string{} : it->second;
}

std::uint64_t CurrentOsThreadId()
{
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
  return static_cast<std::uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

double CurrentThreadCpuSeconds()
{
#if defined(_WIN32)
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetThreadTimes(::GetCurrentThread(), &creation, &exit, &kernel, &user) == 0)
    return -1.0;
  const auto to_seconds = [](const FILETIME& value) {
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    return static_cast<double>(ticks) / 1.0e7;
  };
  return to_seconds(kernel) + to_seconds(user);
#elif defined(__APPLE__)
  mach_port_t port = ::mach_thread_self();
  thread_basic_info_data_t info{};
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  const kern_return_t status =
      ::thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
  ::mach_port_deallocate(::mach_task_self(), port);
  if (status != KERN_SUCCESS)
    return -1.0;
  return static_cast<double>(info.user_time.seconds + info.system_time.seconds) +
         static_cast<double>(info.user_time.microseconds + info.system_time.microseconds) /
             1.0e6;
#else
  timespec ts{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    return -1.0;
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1.0e9;
#endif
}

double ThreadCpuSeconds(std::uint64_t)
{
  // Reading another thread's CPU time portably is not available everywhere;
  // threads sample their own time through CurrentThreadCpuSeconds instead.
  return -1.0;
}

std::string FormatDouble(double value, int decimals)
{
  if (!std::isfinite(value))
    return "0";
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::fixed, decimals);
  if (result.ec != std::errc{})
    return "0";
  return std::string(buffer, result.ptr);
}

std::string EscapeJson(std::string_view text)
{
  std::string out;
  out.reserve(text.size() + 8);
  for (const char raw : text)
  {
    const auto c = static_cast<unsigned char>(raw);
    switch (c)
    {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20)
      {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out += kHex[(c >> 4) & 0xF];
        out += kHex[c & 0xF];
      }
      else
      {
        out += raw;
      }
      break;
    }
  }
  return out;
}

std::string UtcTimestamp()
{
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[32];
  const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buffer, written);
}

JsonWriter::JsonWriter(int indent) : m_indent(indent) {}

void JsonWriter::Separator()
{
  if (m_after_key)
  {
    m_after_key = false;
    return;
  }
  if (m_need_comma)
    m_out += ',';
  if (m_indent > 0 && !m_out.empty())
    m_out += '\n';
  Indent();
}

void JsonWriter::Indent()
{
  if (m_indent <= 0)
    return;
  m_out.append(static_cast<std::size_t>(m_indent * m_depth), ' ');
}

void JsonWriter::BeginObject()
{
  Separator();
  m_out += '{';
  ++m_depth;
  m_need_comma = false;
}

void JsonWriter::EndObject()
{
  --m_depth;
  if (m_indent > 0 && m_need_comma)
  {
    m_out += '\n';
    Indent();
  }
  m_out += '}';
  m_need_comma = true;
}

void JsonWriter::BeginArray()
{
  Separator();
  m_out += '[';
  ++m_depth;
  m_need_comma = false;
}

void JsonWriter::EndArray()
{
  --m_depth;
  if (m_indent > 0 && m_need_comma)
  {
    m_out += '\n';
    Indent();
  }
  m_out += ']';
  m_need_comma = true;
}

void JsonWriter::Key(std::string_view key)
{
  Separator();
  m_out += '"';
  m_out += EscapeJson(key);
  m_out += "\":";
  if (m_indent > 0)
    m_out += ' ';
  m_after_key = true;
}

void JsonWriter::String(std::string_view value)
{
  Separator();
  m_out += '"';
  m_out += EscapeJson(value);
  m_out += '"';
  m_need_comma = true;
}

void JsonWriter::Number(double value)
{
  Separator();
  m_out += FormatDouble(value, 4);
  m_need_comma = true;
}

void JsonWriter::Integer(std::uint64_t value)
{
  Separator();
  m_out += std::to_string(value);
  m_need_comma = true;
}

void JsonWriter::Integer(std::int64_t value)
{
  Separator();
  m_out += std::to_string(value);
  m_need_comma = true;
}

void JsonWriter::Bool(bool value)
{
  Separator();
  m_out += value ? "true" : "false";
  m_need_comma = true;
}

void JsonWriter::Null()
{
  Separator();
  m_out += "null";
  m_need_comma = true;
}

void JsonWriter::KeyString(std::string_view key, std::string_view value)
{
  Key(key);
  String(value);
}

void JsonWriter::KeyNumber(std::string_view key, double value)
{
  Key(key);
  Number(value);
}

void JsonWriter::KeyInteger(std::string_view key, std::uint64_t value)
{
  Key(key);
  Integer(value);
}

void JsonWriter::KeyBool(std::string_view key, bool value)
{
  Key(key);
  Bool(value);
}

std::string JsonWriter::Take()
{
  if (m_indent > 0)
    m_out += '\n';
  return std::move(m_out);
}
}  // namespace detail

const char* LevelName(Level level)
{
  switch (level)
  {
  case Level::Off: return "off";
  case Level::Basic: return "basic";
  case Level::Detailed: return "detailed";
  case Level::Trace: return "trace";
  }
  return "off";
}

bool ParseLevel(std::string_view text, Level* out)
{
  const Level levels[] = {Level::Off, Level::Basic, Level::Detailed, Level::Trace};
  for (const Level level : levels)
  {
    if (text == LevelName(level))
    {
      if (out != nullptr)
        *out = level;
      return true;
    }
  }
  return false;
}

const char* ZoneName(Zone zone)
{
  switch (zone)
  {
  case Zone::GuestCpu: return "GuestCpu";
  case Zone::StaticRecompDispatch: return "StaticRecompDispatch";
  case Zone::InterpreterFallback: return "InterpreterFallback";
  case Zone::GxCommandProcessor: return "GxCommandProcessor";
  case Zone::VertexLoader: return "VertexLoader";
  case Zone::TextureDecoder: return "TextureDecoder";
  case Zone::ShaderGeneration: return "ShaderGeneration";
  case Zone::PipelineCreation: return "PipelineCreation";
  case Zone::RendererSubmission: return "RendererSubmission";
  case Zone::GpuWait: return "GpuWait";
  case Zone::GpuExecution: return "GpuExecution";
  case Zone::Present: return "Present";
  case Zone::Dsp: return "Dsp";
  case Zone::Audio: return "Audio";
  case Zone::Memory: return "Memory";
  case Zone::Mmio: return "Mmio";
  case Zone::Scheduler: return "Scheduler";
  case Zone::Synchronization: return "Synchronization";
  case Zone::Mods: return "Mods";
  case Zone::Netplay: return "Netplay";
  case Zone::Other: return "Other";
  }
  return "Other";
}

const char* CounterName(Counter counter)
{
  switch (counter)
  {
  case Counter::StaticRecompDispatches: return "static_recomp_dispatches";
  case Counter::StaticRecompDispatchMisses: return "static_recomp_dispatch_misses";
  case Counter::InterpreterFallbacks: return "interpreter_fallbacks";
  case Counter::UnsupportedInstructionFallbacks: return "unsupported_instruction_fallbacks";
  case Counter::GuestInstructions: return "guest_instructions";
  case Counter::IndirectBranches: return "indirect_branches";
  case Counter::IndirectBranchFastHits: return "indirect_branch_fast_hits";
  case Counter::IndirectBranchSlowLookups: return "indirect_branch_slow_lookups";
  case Counter::MmioReads: return "mmio_reads";
  case Counter::MmioWrites: return "mmio_writes";
  case Counter::MmioSlowPaths: return "mmio_slow_paths";
  case Counter::Exceptions: return "exceptions";
  case Counter::Interrupts: return "interrupts";
  case Counter::CodeInvalidations: return "code_invalidations";
  case Counter::HostCalls: return "host_calls";
  case Counter::ModHostCalls: return "mod_host_calls";
  case Counter::GxCommands: return "gx_commands";
  case Counter::GxUnknownOpcodes: return "gx_unknown_opcodes";
  case Counter::GxDisplayLists: return "gx_display_lists";
  case Counter::DrawCalls: return "draw_calls";
  case Counter::PrimitivesLoaded: return "primitives_loaded";
  case Counter::VerticesLoaded: return "vertices_loaded";
  case Counter::TextureDecodes: return "texture_decodes";
  case Counter::TextureDecodeBytes: return "texture_decode_bytes";
  case Counter::TextureUploads: return "texture_uploads";
  case Counter::TextureUploadBytes: return "texture_upload_bytes";
  case Counter::BufferUploads: return "buffer_uploads";
  case Counter::EfbReads: return "efb_reads";
  case Counter::EfbWrites: return "efb_writes";
  case Counter::EfbCopies: return "efb_copies";
  case Counter::FramebufferReadbacks: return "framebuffer_readbacks";
  case Counter::PipelineSwitches: return "pipeline_switches";
  case Counter::PipelineCreations: return "pipeline_creations";
  case Counter::ShaderCacheHits: return "shader_cache_hits";
  case Counter::ShaderCacheMisses: return "shader_cache_misses";
  case Counter::ShaderCompilations: return "shader_compilations";
  case Counter::QueueSubmissions: return "queue_submissions";
  case Counter::SynchronizationEvents: return "synchronization_events";
  case Counter::GpuFenceWaits: return "gpu_fence_waits";
  case Counter::SchedulerSleeps: return "scheduler_sleeps";
  case Counter::NetplayInputWaits: return "netplay_input_waits";
  case Counter::AudioUnderruns: return "audio_underruns";
  }
  return "unknown";
}

const char* EventTypeName(EventType type)
{
  switch (type)
  {
  case EventType::CaptureStart: return "capture_start";
  case EventType::CaptureStop: return "capture_stop";
  case EventType::ShaderCompilation: return "shader_compilation";
  case EventType::PipelineCompilation: return "pipeline_compilation";
  case EventType::ShaderCacheMiss: return "shader_cache_miss";
  case EventType::GpuStall: return "gpu_stall";
  case EventType::LongFrame: return "long_frame";
  case EventType::Note: return "note";
  }
  return "note";
}
}  // namespace moderngekko::diagnostics
