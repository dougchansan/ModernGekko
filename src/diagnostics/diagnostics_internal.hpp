#pragma once

#include "moderngekko/diagnostics.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace moderngekko::diagnostics::detail
{
// Thread states live for the process lifetime so a thread that exits mid
// capture still contributes its totals.
std::vector<ThreadState*> RegisteredThreads();
void SetThreadName(ThreadState& state, std::string name);
std::string ThreadName(const ThreadState& state);
std::uint64_t CurrentOsThreadId();
// Seconds of CPU time consumed by the calling thread, or -1 when the platform
// cannot report it.
double CurrentThreadCpuSeconds();
double ThreadCpuSeconds(std::uint64_t os_thread_id);

// Fixed-capacity ring buffer. Never allocates after Reset, and overwrites the
// oldest entry once full. Not thread safe by itself; TraceBuffer guards it.
template <typename T>
class RingBuffer final
{
public:
  void Reset(std::size_t capacity)
  {
    m_items.assign(capacity, T{});
    m_items.shrink_to_fit();
    m_head = 0;
    m_size = 0;
    m_dropped = 0;
  }

  void Clear()
  {
    m_head = 0;
    m_size = 0;
    m_dropped = 0;
  }

  void Push(const T& value)
  {
    if (m_items.empty())
    {
      ++m_dropped;
      return;
    }
    m_items[m_head] = value;
    m_head = (m_head + 1) % m_items.size();
    if (m_size < m_items.size())
      ++m_size;
    else
      ++m_dropped;
  }

  std::size_t Size() const { return m_size; }
  std::size_t Capacity() const { return m_items.size(); }
  std::uint64_t DroppedCount() const { return m_dropped; }

  // Oldest entry first.
  std::vector<T> Snapshot() const
  {
    std::vector<T> out;
    out.reserve(m_size);
    if (m_items.empty())
      return out;
    const std::size_t start = (m_head + m_items.size() - m_size) % m_items.size();
    for (std::size_t i = 0; i < m_size; ++i)
      out.push_back(m_items[(start + i) % m_items.size()]);
    return out;
  }

private:
  std::vector<T> m_items;
  std::size_t m_head = 0;
  std::size_t m_size = 0;
  std::uint64_t m_dropped = 0;
};

// Bounded rolling history of frames and events shared between the frame
// thread and whichever thread writes a report.
class TraceBuffer final
{
public:
  void Reset(std::size_t frame_capacity, std::size_t event_capacity);
  void Clear();

  void PushFrame(const FrameRecord& frame);
  void PushEvent(const Event& event);

  std::vector<FrameRecord> Frames() const;
  std::vector<Event> Events() const;
  std::vector<FrameRecord> RecentFrames(std::size_t count) const;
  std::size_t FrameCount() const;
  std::uint64_t DroppedFrames() const;
  std::uint64_t DroppedEvents() const;

private:
  mutable std::mutex m_mutex;
  RingBuffer<FrameRecord> m_frames;
  RingBuffer<Event> m_events;
};

// A single file inside a .mgdiag archive.
struct ArchiveEntry
{
  std::string name;
  std::string data;
};

// .mgdiag is a ZIP container with stored (uncompressed) entries, so any
// unzip tool can open it without ModernGekko pulling in a compression
// dependency.
bool WriteArchive(const std::filesystem::path& path, const std::vector<ArchiveEntry>& entries,
                  std::string* error);
bool ReadArchive(const std::filesystem::path& path, std::vector<ArchiveEntry>* entries,
                 std::string* error);
std::uint32_t Crc32(std::string_view data);

// Minimal deterministic JSON writer. Keys are emitted in insertion order.
class JsonWriter final
{
public:
  explicit JsonWriter(int indent = 2);

  void BeginObject();
  void EndObject();
  void BeginArray();
  void EndArray();
  void Key(std::string_view key);
  void String(std::string_view value);
  void Number(double value);
  void Integer(std::uint64_t value);
  void Integer(std::int64_t value);
  void Bool(bool value);
  void Null();

  void KeyString(std::string_view key, std::string_view value);
  void KeyNumber(std::string_view key, double value);
  void KeyInteger(std::string_view key, std::uint64_t value);
  void KeyBool(std::string_view key, bool value);

  std::string Take();

private:
  void Separator();
  void Indent();

  std::string m_out;
  int m_indent = 2;
  int m_depth = 0;
  bool m_need_comma = false;
  bool m_after_key = false;
};

std::string EscapeJson(std::string_view text);
// Formats with a fixed number of decimals and no locale dependency.
std::string FormatDouble(double value, int decimals);
std::string UtcTimestamp();
}  // namespace moderngekko::diagnostics::detail
