#include "diagnostics_internal.hpp"

namespace moderngekko::diagnostics::detail
{
void TraceBuffer::Reset(std::size_t frame_capacity, std::size_t event_capacity)
{
  std::lock_guard lock(m_mutex);
  m_frames.Reset(frame_capacity);
  m_events.Reset(event_capacity);
}

void TraceBuffer::Clear()
{
  std::lock_guard lock(m_mutex);
  m_frames.Clear();
  m_events.Clear();
}

void TraceBuffer::PushFrame(const FrameRecord& frame)
{
  std::lock_guard lock(m_mutex);
  m_frames.Push(frame);
}

void TraceBuffer::PushEvent(const Event& event)
{
  std::lock_guard lock(m_mutex);
  m_events.Push(event);
}

std::vector<FrameRecord> TraceBuffer::Frames() const
{
  std::lock_guard lock(m_mutex);
  return m_frames.Snapshot();
}

std::vector<Event> TraceBuffer::Events() const
{
  std::lock_guard lock(m_mutex);
  return m_events.Snapshot();
}

std::vector<FrameRecord> TraceBuffer::RecentFrames(std::size_t count) const
{
  std::lock_guard lock(m_mutex);
  std::vector<FrameRecord> all = m_frames.Snapshot();
  if (all.size() <= count)
    return all;
  return std::vector<FrameRecord>(all.end() - static_cast<std::ptrdiff_t>(count), all.end());
}

std::size_t TraceBuffer::FrameCount() const
{
  std::lock_guard lock(m_mutex);
  return m_frames.Size();
}

std::uint64_t TraceBuffer::DroppedFrames() const
{
  std::lock_guard lock(m_mutex);
  return m_frames.DroppedCount();
}

std::uint64_t TraceBuffer::DroppedEvents() const
{
  std::lock_guard lock(m_mutex);
  return m_events.DroppedCount();
}
}  // namespace moderngekko::diagnostics::detail
