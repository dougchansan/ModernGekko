#include "moderngekko/runtime.hpp"
#include "moderngekko/hd_texture_pack.hpp"

#include "automation_protocol.hpp"
#include "pause_menu_host.hpp"
#include "launcher_savestates.hpp"
#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IniFile.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Cheats/ActionReplay.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/State.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GCPad.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Common/Swap.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/module_loader.hpp"
#include "InputCommon/GCPadStatus.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <fmt/format.h>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <imgui.h>
#include <climits>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;
std::atomic<int> s_native_ultrawide_width{5120};
std::atomic<int> s_native_ultrawide_height{1440};

struct NavigationPoint
{
  float x = 0.0f;
  float z = 0.0f;
};

struct NavigationActor
{
  NavigationPoint position;
  u32 slot = 0;
  u32 archetype = 0;
  u32 script_state = 0;
  bool scripted = false;
};

enum class NavigationSegmentKind : u8
{
  Walkable,
  Wall,
  Trigger,
};

struct NavigationSegment
{
  NavigationPoint from;
  NavigationPoint to;
  NavigationSegmentKind kind = NavigationSegmentKind::Wall;
};

struct NavigationOverlayState
{
  std::mutex mutex;
  std::atomic<float> player_x{0.0f};
  std::atomic<float> player_z{0.0f};
  std::atomic<float> player_facing{0.0f};
  std::atomic<float> companion_x{0.0f};
  std::atomic<float> companion_z{0.0f};
  std::atomic<bool> companion_valid{false};
  std::atomic<unsigned int> room_id{0};
  std::atomic<bool> valid{false};
  std::atomic<bool> overlay_visible{true};
  std::atomic<bool> widescreen_enabled{false};
  std::atomic<bool> hires_textures_enabled{false};
  std::atomic<bool> community_hd_texture_pack_enabled{false};
  std::atomic<bool> dump_textures_enabled{false};
  std::atomic<bool> text_upscale_enabled{false};
  std::atomic<bool> input_overlay_enabled{false};
  std::atomic<bool> minimap_high_contrast{false};
  std::atomic<int> accessibility_ui_scale{100};
  std::atomic<bool> sixty_fps_enabled{false};
  std::atomic<bool> fast_forward_enabled{false};
  std::atomic<bool> fast_forward_active{false};
  std::atomic<int> fast_forward_multiplier{2};
  std::atomic<bool> autosave_enabled{true};
  std::atomic<int> autosave_slots{5};
  std::atomic<bool> menu_active{false};
  std::atomic<bool> authored_menu_active{false};
  std::atomic<bool> authored_projection_active{false};
  std::atomic<bool> movie_active{false};
  std::atomic<bool> live_title_scene_active{false};
  std::atomic<int> authored_backdrop{0};
  std::atomic<bool> pda_active{false};
  std::atomic<bool> field_overlay_active{false};
  std::atomic<u32> active_ui_message_id{0};
  std::atomic<u32> field_overlay_mode{0};
  std::atomic<u32> field_overlay_phase{0};
  std::atomic<u32> dialogue_object{0};
  std::atomic<bool> fit_map_requested{true};
  std::atomic<int> render_surface_width{0};
  std::atomic<int> render_surface_height{0};
  std::atomic<float> render_surface_aspect{4.0f / 3.0f};
  std::atomic<unsigned int> render_efb_scale{1};
  std::atomic<int> ultrawide_efb_scale{5};
  std::vector<NavigationActor> npcs;
  std::vector<NavigationSegment> collision_segments;
  bool collision_valid = false;
  // Bookkeeping for the player-position fallback used when the field actor
  // table has no player record (interiors). CPU-thread only.
  float last_fallback_x = 0.0f;
  float last_fallback_z = 0.0f;
  float last_fallback_facing = 0.0f;
  std::atomic<bool> ultrawide_gameplay_started{false};
  bool menu_aspect_active = false;
  bool menu_projection_active = false;
  bool naming_presentation_active = false;
  bool field_aspect_active = false;
  bool movie_aspect_active = false;
  std::atomic<bool> menu_edge_fill_active{false};
  // Presentation telemetry. The mirrors above are CPU-thread-only bookkeeping;
  // these publish the same decisions so automation can observe what was
  // actually presented. The counter is monotonic so a sampler cannot miss a
  // transition by polling more slowly than the field rate.
  // Set when something outside the per-field presentation block writes the
  // Colosseum config bits, so the next field re-applies instead of trusting a
  // cache that no longer describes what Config actually holds.
  std::atomic<bool> presentation_cache_dirty{false};
  // Debounce. The naming detector reads two guest words (0x80402436 and
  // 0x8040836C) that the game does not update in the same field, so entering
  // and leaving an authored menu produces one or two fields where they
  // disagree. Applying those immediately flips the aspect and flips it back:
  // the flicker. Require the observation to hold before acting on it.
  bool pending_menu_aspect = false;
  bool pending_menu_projection = false;
  bool pending_naming_presentation = false;
  bool pending_field_aspect = false;
  bool pending_movie_aspect = false;
  bool pending_menu_edge_fill = false;
  int presentation_stable_fields = 0;
  // Fields remaining before the naming state is released. The confirmation
  // dialog blinks its UI message, so the raw predicate cannot be trusted from
  // one field to the next.
  int naming_hold_fields = 0;
  // Guards against a signal that never holds still: without this, a tuple that
  // keeps changing would never reach the stable threshold and the presented
  // state would stay frozen at whatever it held when the game loaded.
  int presentation_fields_since_apply = 0;
  // The guest widescreen multiplier, decided once on the CPU thread from the
  // debounced state. The video thread enforces this value rather than deriving
  // its own: this word scales the 3D model, so two threads sampling the flags
  // at different instants made the character visibly flicker between aspects.
  std::atomic<u32> guest_projection_bits{std::bit_cast<u32>(1.0f)};
  std::atomic<bool> published_menu_active{false};
  std::atomic<bool> published_naming_presentation{false};
  std::atomic<bool> published_gameplay_ultrawide{false};
  std::atomic<int> published_aspect_mode{-1};
  std::atomic<std::uint64_t> presentation_transitions{0};
  bool field_overlay_session_active = false;
  bool battle_gameplay_session_active = false;
  unsigned int battle_now_arena_room = 0;
  int battle_gameplay_message_fields = 0;
  int battle_now_hold_fields = 0;
  int front_end_dismiss_hold_fields = 0;
  u32 pyrite_sky_root = 0;
  u32 pyrite_sky_original_translate_y = 0;
  bool pyrite_sky_offset_active = false;
  unsigned int collision_room_id = std::numeric_limits<unsigned int>::max();
  int collision_refresh_delay_frames = 0;
  int collision_refresh_retries = 0;
  u32 collision_base = 0;
  std::string collision_archive_name;
  std::string location_name;
  float collision_min_x = 0.0f;
  float collision_max_x = 0.0f;
  float collision_min_z = 0.0f;
  float collision_max_z = 0.0f;
  unsigned int displayed_room_id = 0;
  std::filesystem::path savestate_directory;
  std::filesystem::path frontend_config_path;
  std::filesystem::path pending_save_state;
  std::filesystem::path pending_load_state;
  bool pending_save_is_recovery = false;
  bool has_seen_valid_room = false;
  GCPadStatus input_status{};
  // Queued like the save/load paths rather than acted on in the window
  // procedure: these have to happen on the thread that owns the core, and the
  // UI thread does not.
  bool pending_reset = false;
  bool pending_pause_toggle = false;
  bool pending_mute_toggle = false;
  // Mirrors Config::MAIN_AUDIO_MUTED so the menu can show a checkmark without
  // reading Dolphin config from the window procedure.
  std::atomic<bool> audio_muted{false};
  std::atomic<bool> paused{false};
  std::string ui_status;
  int standard_efb_scale = 2;
  std::atomic<int> mode{0};
};

std::atomic<NavigationOverlayState*> s_navigation_overlay_state{nullptr};

float ReadGuestFloat(const Memory::MemoryManager& memory, u32 address)
{
  return std::bit_cast<float>(memory.Read_U32(address));
}

void UpdateColosseumPyriteSkyCoverage(Memory::MemoryManager& memory,
                                      NavigationOverlayState& state,
                                      bool should_offset)
{
  constexpr u32 pyrite_sky_root_address = 0x8050FDB8;
  constexpr u32 jobj_translate_y_offset = 0x3c;
  constexpr u32 pyrite_sky_translate_y = 0xc3960000; // -300.0f
  constexpr u32 mem1_end = 0x81800000;

  const u32 current_root = memory.Read_U32(pyrite_sky_root_address);
  const auto restore_tracked_root = [&] {
    if (state.pyrite_sky_offset_active && current_root == state.pyrite_sky_root &&
        current_root >= Memory::MEM1_BASE_ADDR && current_root < mem1_end &&
        memory.Read_U32(current_root + jobj_translate_y_offset) ==
            pyrite_sky_translate_y)
    {
      memory.Write_U32(state.pyrite_sky_original_translate_y,
                       current_root + jobj_translate_y_offset);
    }
    state.pyrite_sky_root = 0;
    state.pyrite_sky_original_translate_y = 0;
    state.pyrite_sky_offset_active = false;
  };

  if (!should_offset || current_root < Memory::MEM1_BASE_ADDR ||
      current_root >= mem1_end)
  {
    restore_tracked_root();
    return;
  }

  if (state.pyrite_sky_offset_active && state.pyrite_sky_root != current_root)
    restore_tracked_root();

  if (!state.pyrite_sky_offset_active)
  {
    state.pyrite_sky_root = current_root;
    state.pyrite_sky_original_translate_y =
        memory.Read_U32(current_root + jobj_translate_y_offset);
    state.pyrite_sky_offset_active = true;
  }

  if (memory.Read_U32(current_root + jobj_translate_y_offset) !=
      pyrite_sky_translate_y)
  {
    memory.Write_U32(pyrite_sky_translate_y,
                     current_root + jobj_translate_y_offset);
  }
}

struct CollisionVec3
{
  float x;
  float y;
  float z;
};

struct CollisionTransform
{
  CollisionVec3 translation;
  CollisionVec3 rotation;
  CollisionVec3 scale;
};

struct CollisionMapCandidate
{
  std::vector<NavigationSegment> segments;
  u32 base = 0;
  std::string archive_name;
  float min_x = 0.0f;
  float max_x = 0.0f;
  float min_z = 0.0f;
  float max_z = 0.0f;
  int score = std::numeric_limits<int>::min();
};

u32 ReadBigEndianU32(std::span<const u8> ram, u32 offset)
{
  return (static_cast<u32>(ram[offset]) << 24) |
         (static_cast<u32>(ram[offset + 1]) << 16) |
         (static_cast<u32>(ram[offset + 2]) << 8) |
         static_cast<u32>(ram[offset + 3]);
}

bool GuestRangeToOffset(std::span<const u8> ram, u32 address, std::size_t size,
                        u32* offset)
{
  if (address < Memory::MEM1_BASE_ADDR)
    return false;
  const u64 candidate = static_cast<u64>(address) - Memory::MEM1_BASE_ADDR;
  if (candidate + size > ram.size())
    return false;
  *offset = static_cast<u32>(candidate);
  return true;
}

bool ReadCollisionU32(std::span<const u8> ram, u32 address, u32* value)
{
  u32 offset;
  if (!GuestRangeToOffset(ram, address, sizeof(u32), &offset))
    return false;
  *value = ReadBigEndianU32(ram, offset);
  return true;
}

bool ReadCollisionFloat(std::span<const u8> ram, u32 address, float* value)
{
  u32 bits;
  if (!ReadCollisionU32(ram, address, &bits))
    return false;
  *value = std::bit_cast<float>(bits);
  return std::isfinite(*value);
}

float CollisionAngleToRadians(float angle)
{
  constexpr float pi = 3.14159265358979323846f;
  if (std::abs(angle) > pi * 2.0f + 0.01f)
    return angle * (pi / 180.0f);
  return angle;
}

CollisionVec3 TransformCollisionPoint(const CollisionTransform& transform,
                                      CollisionVec3 point)
{
  point.x *= transform.scale.x;
  point.y *= transform.scale.y;
  point.z *= transform.scale.z;

  const float rx = CollisionAngleToRadians(transform.rotation.x);
  const float ry = CollisionAngleToRadians(transform.rotation.y);
  const float rz = CollisionAngleToRadians(transform.rotation.z);
  const float sin_x = std::sin(rx);
  const float cos_x = std::cos(rx);
  const float sin_y = std::sin(ry);
  const float cos_y = std::cos(ry);
  const float sin_z = std::sin(rz);
  const float cos_z = std::cos(rz);

  point = {point.x, point.y * cos_x - point.z * sin_x,
           point.y * sin_x + point.z * cos_x};
  point = {point.x * cos_y + point.z * sin_y, point.y,
           -point.x * sin_y + point.z * cos_y};
  point = {point.x * cos_z - point.y * sin_z,
           point.x * sin_z + point.y * cos_z, point.z};
  point.x += transform.translation.x;
  point.y += transform.translation.y;
  point.z += transform.translation.z;
  return point;
}

using CollisionSegmentKey = std::tuple<int, int, int, int, NavigationSegmentKind>;

CollisionSegmentKey MakeCollisionSegmentKey(NavigationPoint from, NavigationPoint to,
                                            NavigationSegmentKind kind)
{
  constexpr float quantization = 10.0f;
  int from_x = static_cast<int>(std::lround(from.x * quantization));
  int from_z = static_cast<int>(std::lround(from.z * quantization));
  int to_x = static_cast<int>(std::lround(to.x * quantization));
  int to_z = static_cast<int>(std::lround(to.z * quantization));
  if (std::tie(to_x, to_z) < std::tie(from_x, from_z))
  {
    std::swap(from_x, to_x);
    std::swap(from_z, to_z);
  }
  return {from_x, from_z, to_x, to_z, kind};
}

struct CollisionSegmentCount
{
  NavigationSegment segment;
  unsigned int count = 0;
};

bool AddCollisionTriangleModel(
    std::span<const u8> ram, u32 model_address, const CollisionTransform& transform,
    NavigationSegmentKind kind,
    std::map<CollisionSegmentKey, CollisionSegmentCount>* segment_counts)
{
  u32 triangle_address;
  u32 triangle_count;
  if (!ReadCollisionU32(ram, model_address, &triangle_address) ||
      !ReadCollisionU32(ram, model_address + 4, &triangle_count) ||
      triangle_count == 0 || triangle_count > 16384)
  {
    return false;
  }

  constexpr u32 triangle_size = 0x34;
  u32 triangle_offset;
  if (!GuestRangeToOffset(ram, triangle_address,
                          static_cast<std::size_t>(triangle_count) * triangle_size,
                          &triangle_offset))
  {
    return false;
  }

  for (u32 triangle_index = 0; triangle_index < triangle_count; ++triangle_index)
  {
    const u32 triangle = triangle_address + triangle_index * triangle_size;
    std::array<CollisionVec3, 3> vertices{};
    bool valid = true;
    for (std::size_t vertex_index = 0; vertex_index < vertices.size(); ++vertex_index)
    {
      const u32 vertex = triangle + static_cast<u32>(vertex_index) * 12;
      valid &= ReadCollisionFloat(ram, vertex, &vertices[vertex_index].x);
      valid &= ReadCollisionFloat(ram, vertex + 4, &vertices[vertex_index].y);
      valid &= ReadCollisionFloat(ram, vertex + 8, &vertices[vertex_index].z);
      vertices[vertex_index] =
          TransformCollisionPoint(transform, vertices[vertex_index]);
    }
    if (!valid)
      continue;

    for (std::size_t edge_index = 0; edge_index < vertices.size(); ++edge_index)
    {
      const CollisionVec3& from_vertex = vertices[edge_index];
      const CollisionVec3& to_vertex = vertices[(edge_index + 1) % vertices.size()];
      const NavigationPoint from{from_vertex.x, from_vertex.z};
      const NavigationPoint to{to_vertex.x, to_vertex.z};
      if (!std::isfinite(from.x) || !std::isfinite(from.z) ||
          !std::isfinite(to.x) || !std::isfinite(to.z) ||
          std::hypot(to.x - from.x, to.z - from.z) < 0.05f)
      {
        continue;
      }

      const CollisionSegmentKey key = MakeCollisionSegmentKey(from, to, kind);
      CollisionSegmentCount& entry = (*segment_counts)[key];
      if (entry.count == 0)
        entry.segment = {from, to, kind};
      ++entry.count;
    }
  }
  return true;
}

bool BuildCollisionMapCandidate(std::span<const u8> ram, u32 base, float player_x,
                                float player_z, CollisionMapCandidate* candidate)
{
  u32 descriptor_address;
  u32 descriptor_count;
  if (!ReadCollisionU32(ram, base, &descriptor_address) ||
      descriptor_address != base + 0x10 ||
      !ReadCollisionU32(ram, base + 4, &descriptor_count) ||
      descriptor_count == 0 || descriptor_count > 256)
  {
    return false;
  }

  u32 descriptor_offset;
  if (!GuestRangeToOffset(ram, descriptor_address,
                          static_cast<std::size_t>(descriptor_count) * 0x40,
                          &descriptor_offset))
  {
    return false;
  }

  std::map<CollisionSegmentKey, CollisionSegmentCount> segment_counts;
  bool found_model = false;
  for (u32 descriptor_index = 0; descriptor_index < descriptor_count;
       ++descriptor_index)
  {
    const u32 descriptor = descriptor_address + descriptor_index * 0x40;
    CollisionTransform transform{};
    bool valid_transform = true;
    valid_transform &=
        ReadCollisionFloat(ram, descriptor, &transform.translation.x);
    valid_transform &=
        ReadCollisionFloat(ram, descriptor + 4, &transform.translation.y);
    valid_transform &=
        ReadCollisionFloat(ram, descriptor + 8, &transform.translation.z);
    valid_transform &= ReadCollisionFloat(ram, descriptor + 12, &transform.rotation.x);
    valid_transform &= ReadCollisionFloat(ram, descriptor + 16, &transform.rotation.y);
    valid_transform &= ReadCollisionFloat(ram, descriptor + 20, &transform.rotation.z);
    valid_transform &= ReadCollisionFloat(ram, descriptor + 24, &transform.scale.x);
    valid_transform &= ReadCollisionFloat(ram, descriptor + 28, &transform.scale.y);
    valid_transform &= ReadCollisionFloat(ram, descriptor + 32, &transform.scale.z);
    if (!valid_transform || std::abs(transform.scale.x) < 0.0001f ||
        std::abs(transform.scale.y) < 0.0001f ||
        std::abs(transform.scale.z) < 0.0001f ||
        std::abs(transform.scale.x) > 1000.0f ||
        std::abs(transform.scale.y) > 1000.0f ||
        std::abs(transform.scale.z) > 1000.0f)
    {
      return false;
    }

    constexpr std::array<NavigationSegmentKind, 5> model_kinds = {
        NavigationSegmentKind::Walkable, NavigationSegmentKind::Wall,
        NavigationSegmentKind::Trigger, NavigationSegmentKind::Trigger,
        NavigationSegmentKind::Wall};
    for (std::size_t model_index = 0; model_index < model_kinds.size(); ++model_index)
    {
      u32 model_address;
      if (!ReadCollisionU32(ram, descriptor + 0x24 +
                                     static_cast<u32>(model_index) * 4,
                            &model_address))
      {
        return false;
      }
      if (model_address == 0)
        continue;

      const u64 first_model_address =
          static_cast<u64>(descriptor_address) + descriptor_count * 0x40;
      if (model_address < first_model_address ||
          static_cast<u64>(model_address) >= static_cast<u64>(base) + 0x800000)
      {
        return false;
      }
      found_model |= AddCollisionTriangleModel(ram, model_address, transform,
                                               model_kinds[model_index],
                                               &segment_counts);
    }
  }
  if (!found_model)
    return false;

  std::vector<NavigationSegment> segments;
  segments.reserve(segment_counts.size());
  for (const auto& [key, value] : segment_counts)
  {
    if (value.segment.kind != NavigationSegmentKind::Walkable || value.count == 1)
      segments.push_back(value.segment);
  }
  if (segments.size() < 3 || segments.size() > 16384)
    return false;

  auto calculate_bounds = [&segments](bool include_walkable, float* min_x,
                                      float* max_x, float* min_z, float* max_z) {
    *min_x = std::numeric_limits<float>::max();
    *max_x = std::numeric_limits<float>::lowest();
    *min_z = std::numeric_limits<float>::max();
    *max_z = std::numeric_limits<float>::lowest();
    bool found = false;
    for (const NavigationSegment& segment : segments)
    {
      if (!include_walkable && segment.kind == NavigationSegmentKind::Walkable)
        continue;
      *min_x = std::min({*min_x, segment.from.x, segment.to.x});
      *max_x = std::max({*max_x, segment.from.x, segment.to.x});
      *min_z = std::min({*min_z, segment.from.z, segment.to.z});
      *max_z = std::max({*max_z, segment.from.z, segment.to.z});
      found = true;
    }
    return found;
  };

  float min_x;
  float max_x;
  float min_z;
  float max_z;
  if (!calculate_bounds(false, &min_x, &max_x, &min_z, &max_z) &&
      !calculate_bounds(true, &min_x, &max_x, &min_z, &max_z))
  {
    return false;
  }
  const float width = max_x - min_x;
  const float depth = max_z - min_z;
  if (!std::isfinite(width) || !std::isfinite(depth) || width < 0.1f ||
      depth < 0.1f || width > 100000.0f || depth > 100000.0f)
  {
    return false;
  }

  int wall_count = 0;
  for (const NavigationSegment& segment : segments)
  {
    if (segment.kind == NavigationSegmentKind::Wall)
      ++wall_count;
  }
  const bool contains_player =
      player_x >= min_x - 10.0f && player_x <= max_x + 10.0f &&
      player_z >= min_z - 10.0f && player_z <= max_z + 10.0f;
  candidate->segments = std::move(segments);
  candidate->base = base;
  candidate->min_x = min_x;
  candidate->max_x = max_x;
  candidate->min_z = min_z;
  candidate->max_z = max_z;
  candidate->score = (contains_player ? 100000 : 0) + wall_count * 10 +
                     static_cast<int>(candidate->segments.size());
  return true;
}

std::string FindCollisionArchiveName(std::span<const u8> ram,
                                     u32 collision_base)
{
  constexpr u32 fsys_magic = 0x46535953;
  constexpr u32 max_search_size = 0x800000;
  const u32 collision_offset = collision_base - Memory::MEM1_BASE_ADDR;
  const u32 first_offset =
      collision_offset > max_search_size ? collision_offset - max_search_size : 0;
  for (u32 offset = collision_offset & ~3U; offset >= first_offset; offset -= 4)
  {
    if (ReadBigEndianU32(ram, offset) != fsys_magic || offset + 0x24 > ram.size())
    {
      if (offset < first_offset + 4)
        break;
      continue;
    }

    const u32 archive_size = ReadBigEndianU32(ram, offset + 0x20);
    const u32 offset_table = ReadBigEndianU32(ram, offset + 0x18);
    if (archive_size < 0x40 || archive_size > 0x4000000 ||
        static_cast<u64>(offset) + archive_size <= collision_offset ||
        static_cast<u64>(offset) + offset_table + 8 > ram.size())
    {
      continue;
    }

    const u32 string_table =
        ReadBigEndianU32(ram, offset + offset_table + 4);
    const u64 name_offset = static_cast<u64>(offset) + string_table;
    if (name_offset >= ram.size())
      continue;

    std::string name;
    for (u64 index = name_offset; index < ram.size() && name.size() < 63; ++index)
    {
      const char character = static_cast<char>(ram[index]);
      if (character == '\0')
        break;
      if (!(std::isalnum(static_cast<unsigned char>(character)) ||
            character == '_' || character == '-'))
      {
        name.clear();
        break;
      }
      name.push_back(character);
    }
    if (!name.empty())
      return name;
  }
  return {};
}

std::string FindLoadedFieldArchiveName(std::span<const u8> ram)
{
  constexpr u32 fsys_magic = 0x46535953;
  for (u32 offset = 0; offset + 0x74 <= ram.size(); offset += 4)
  {
    if (ReadBigEndianU32(ram, offset) != fsys_magic)
      continue;

    const u32 archive_size = ReadBigEndianU32(ram, offset + 0x20);
    if (archive_size < 0x80 ||
        static_cast<u64>(offset) + archive_size > ram.size())
    {
      continue;
    }

    std::string name;
    for (u32 index = offset + 0x70;
         index < ram.size() && name.size() < 63; ++index)
    {
      const char character = static_cast<char>(ram[index]);
      if (character == '\0')
        break;
      if (!(std::isalnum(static_cast<unsigned char>(character)) ||
            character == '_' || character == '-' || character == '.'))
      {
        name.clear();
        break;
      }
      name.push_back(character);
    }
    if (name.ends_with(".fsys"))
      name.resize(name.size() - 5);
    if (name.size() >= 4 &&
        (name[0] == 'S' || name[0] == 'M' || name[0] == 'D' ||
         name[0] == 'T') &&
        std::isdigit(static_cast<unsigned char>(name[1])) && name[2] == '_')
    {
      return name;
    }
  }
  return {};
}

std::string FriendlyLocationName(const std::string& archive_name,
                                 unsigned int room_id)
{
  if (archive_name == "S1_out")
    return "Outskirt Stand";
  if (archive_name == "S1_shop_1F")
    return "Outskirt Stand Shop 1F";
  if (archive_name == "M1_out")
    return "Phenac City";
  if (archive_name == "M1_stadium_1F")
    return "Phenac Stadium 1F";

  if (!archive_name.empty())
  {
    std::string name = archive_name;
    std::ranges::replace(name, '_', ' ');
    if (name.size() > 3 && std::isalpha(static_cast<unsigned char>(name[0])) &&
        std::isdigit(static_cast<unsigned char>(name[1])) && name[2] == ' ')
    {
      name.erase(0, 3);
    }
    if (!name.empty())
      name[0] = static_cast<char>(
          std::toupper(static_cast<unsigned char>(name[0])));
    return name;
  }

  return fmt::format("Room {:02X}", room_id);
}

void RefreshNavigationCollisionMap(const Memory::MemoryManager& memory,
                                   NavigationOverlayState& state,
                                   unsigned int room_id, float player_x,
                                   float player_z)
{
  const std::span<const u8> full_ram = memory.GetSpanForAddress(Memory::MEM1_BASE_ADDR);
  const std::size_t ram_size =
      std::min<std::size_t>(memory.GetRamSizeReal(), full_ram.size());
  const std::span<const u8> ram = full_ram.first(ram_size);
  CollisionMapCandidate best;

  constexpr u32 scan_start = 0x00010000;
  for (u32 offset = scan_start; offset + 0x50 <= ram.size(); offset += 4)
  {
    const u32 guest_address = Memory::MEM1_BASE_ADDR + offset;
    if (ReadBigEndianU32(ram, offset) != guest_address + 0x10)
      continue;

    CollisionMapCandidate candidate;
    if (BuildCollisionMapCandidate(ram, guest_address, player_x, player_z, &candidate) &&
        candidate.score > best.score)
    {
      best = std::move(candidate);
    }
  }
  if (best.base != 0)
    best.archive_name = FindCollisionArchiveName(ram, best.base);
  if (best.archive_name.empty())
    best.archive_name = FindLoadedFieldArchiveName(ram);

  std::lock_guard lock(state.mutex);
  state.collision_room_id = room_id;
  state.collision_segments = std::move(best.segments);
  state.collision_valid = !state.collision_segments.empty();
  if (!state.collision_valid)
  {
    // A room change fires this scan the instant the new room id appears, which
    // is before the field archive has finished streaming in -- the scan then
    // finds nothing, and because the room id is now latched nothing ever looks
    // again. That is what left Phenac City with no walls on the minimap after
    // walking out of the Pokemon Center, and with no walls there is nothing to
    // route around, so the water stops being an obstacle.
    //
    // Arm the same retry the savestate path uses. The room id still latches, so
    // a genuinely collision-less scene (a cutscene) costs a bounded handful of
    // rescans rather than a full-RAM scan every frame.
    state.collision_refresh_delay_frames = 30;
    state.collision_refresh_retries = 6;
  }
  else
  {
    // Found it -- stop any retry still pending so a successful scan does not
    // keep paying for the next several.
    state.collision_refresh_retries = 0;
  }
  state.collision_base = best.base;
  state.collision_archive_name = std::move(best.archive_name);
  state.location_name =
      FriendlyLocationName(state.collision_archive_name, room_id);
  state.collision_min_x = best.min_x;
  state.collision_max_x = best.max_x;
  state.collision_min_z = best.min_z;
  state.collision_max_z = best.max_z;
}

std::vector<std::filesystem::path> ListNavigationSavestates(
    const std::filesystem::path& directory)
{
  std::vector<std::filesystem::path> paths;
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec))
    return paths;

  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, ec))
  {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".sav")
      paths.push_back(entry.path());
  }
  std::ranges::sort(paths, [](const auto& left, const auto& right) {
    std::error_code left_ec;
    std::error_code right_ec;
    return std::filesystem::last_write_time(left, left_ec) >
           std::filesystem::last_write_time(right, right_ec);
  });
  return paths;
}

std::filesystem::path MakeNavigationSavestatePath(
    const std::filesystem::path& directory)
{
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const std::time_t now = std::time(nullptr);
  std::tm local_time{};
#ifdef _WIN32
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif
  std::array<char, 32> timestamp{};
  std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d-%H%M%S",
                &local_time);
  return directory / fmt::format("nav-{}.sav", timestamp.data());
}

std::filesystem::path MakeRecoverySavestatePath(
    const std::filesystem::path& directory, unsigned int room_id)
{
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const std::time_t now = std::time(nullptr);
  std::tm local_time{};
#ifdef _WIN32
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif
  std::array<char, 32> timestamp{};
  std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d-%H%M%S",
                &local_time);
  return directory /
         fmt::format("recovery-room{:02X}-{}.sav", room_id, timestamp.data());
}

struct NativeUltrawideTarget
{
  int width = 640;
  int height = 480;
  float aspect = 4.0f / 3.0f;
  unsigned int efb_scale = 1;
};

NativeUltrawideTarget GetNativeUltrawideTarget()
{
  NativeUltrawideTarget target;
  if (g_presenter)
  {
    target.width = std::max(g_presenter->GetBackbufferWidth(), 1);
    target.height = std::max(g_presenter->GetBackbufferHeight(), 1);
    target.aspect = std::clamp(static_cast<float>(target.width) /
                                   static_cast<float>(target.height),
                               1.0f, 4.0f);
    const int configured_efb_scale = Config::Get(Config::GFX_EFB_SCALE);
    target.efb_scale = configured_efb_scale == EFB_SCALE_AUTO_INTEGRAL
                           ? std::max(g_presenter->AutoIntegralScale(), 1u)
                           : static_cast<unsigned int>(
                                 std::max(configured_efb_scale, 1));
  }
  // DolphinNoGUI may expose the game's intermediate 16:9 backbuffer even
  // while an ultrawide window owns the physical display. Keep the requested
  // render target separate from exclusive display-mode selection so headed
  // and automation sessions use the same Hor+ projection.
  const int requested_width =
      s_native_ultrawide_width.load(std::memory_order_relaxed);
  const int requested_height =
      s_native_ultrawide_height.load(std::memory_order_relaxed);
  if (requested_width > 0 && requested_height > 0)
  {
    const float requested_aspect =
        std::clamp(static_cast<float>(requested_width) /
                       static_cast<float>(requested_height),
                   1.0f, 4.0f);
    if (requested_aspect > target.aspect)
    {
      target.width = requested_width;
      target.height = requested_height;
      target.aspect = requested_aspect;
    }
  }
  return target;
}

float NativeHudSafeAreaScale(const NativeUltrawideTarget& target)
{
  return std::clamp((16.0f / 9.0f) / target.aspect, 0.25f, 1.0f);
}

bool SetNavigationWidescreenEnabled(bool enabled, std::string* status)
{
  auto& sconfig = SConfig::GetInstance();
  Common::IniFile global_ini = sconfig.LoadDefaultGameIni();
  Common::IniFile local_ini = sconfig.LoadLocalGameIni();
  int requested_width = 5120;
  int requested_height = 1440;
  const Common::IniFile::Section* const moderngekko_section =
      local_ini.GetSection("ModernGekko");
  if (moderngekko_section)
  {
    moderngekko_section->Get("NativeUltrawideWidth", &requested_width, 0);
    moderngekko_section->Get("NativeUltrawideHeight", &requested_height, 0);
  }
  s_native_ultrawide_width.store(requested_width, std::memory_order_relaxed);
  s_native_ultrawide_height.store(requested_height, std::memory_order_relaxed);
  std::vector<ActionReplay::ARCode> codes =
      ActionReplay::LoadCodes(global_ini, local_ini);
  const auto stock_code =
      std::ranges::find(codes, std::string("16:9 Widescreen"),
                        &ActionReplay::ARCode::name);
  if (stock_code == codes.end())
  {
    if (status)
      *status = "Widescreen projection patch was not found";
    return false;
  }

  // Keep the proven three-instruction guest hook for every aspect ratio. The
  // static recompiler knows this code cave, while replacing it at runtime can
  // introduce an uncompiled branch target. Only its projection multiplier is
  // changed: that preserves the authored vertical FOV and character scale,
  // expanding the world horizontally (Hor+) on ultrawide displays.
  stock_code->enabled = true;
  ActionReplay::ApplyCodes(codes, sconfig.GetGameID(), sconfig.GetRevision());
  ActionReplay::SaveCodes(&local_ini, codes);
  local_ini.GetOrCreateSection("ModernGekko")
      ->Set("NativeUltrawide", enabled);
  const std::string ini_path =
      File::GetUserPath(D_GAMESETTINGS_IDX) + sconfig.GetGameID() + ".ini";
  if (!local_ini.Save(ini_path))
  {
    if (status)
      *status = "Could not save the widescreen setting";
    return false;
  }

  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  const int standard_efb_scale = state ? state->standard_efb_scale : 2;
  const int ultrawide_efb_scale =
      state ? state->ultrawide_efb_scale.load(std::memory_order_relaxed) : 5;
  // The game-owned perspective patch preserves scene culling and the authored
  // vertical field of view. Renderer correction supplies a centered safe area
  // for 2D layers and repairs only cached 16:9 perspective signatures.
  Config::SetCurrent(Config::GFX_WIDESCREEN_HACK, false);
  const bool gameplay_ultrawide =
      enabled && state && state->ultrawide_gameplay_started.load(
                              std::memory_order_relaxed);
  Config::SetCurrent(Config::GFX_WIDESCREEN_HUD_SAFE_AREA,
                     gameplay_ultrawide);
  Config::SetCurrent(Config::GFX_WIDESCREEN_AUTHORED_MENU, false);
  Config::SetCurrent(Config::GFX_COLOSSEUM_NAMING_PRESENTATION, false);
  Config::SetCurrent(
      Config::GFX_WIDESCREEN_HUD_SAFE_AREA_SCALE,
      gameplay_ultrawide ? NativeHudSafeAreaScale(GetNativeUltrawideTarget()) :
                           1.0f);
  Config::SetCurrent(Config::GFX_EFB_SCALE,
                     enabled ? ultrawide_efb_scale : standard_efb_scale);
  Config::SetCurrent(Config::GFX_ASPECT_RATIO,
                     gameplay_ultrawide ? AspectMode::Stretch :
                                          AspectMode::ForceWide);
  // This runs on the host thread and has just overwritten the authored-menu and
  // naming bits unconditionally. Without invalidating the cache, the per-field
  // block would compare against mirrors that still claim the old values, see no
  // change, and leave an authored menu presented at the wrong aspect
  // indefinitely rather than for a frame.
  if (state)
    state->presentation_cache_dirty.store(true, std::memory_order_release);
  if (status)
    *status = enabled ? "Native ultrawide enabled" :
                        "Classic 30-degree camera enabled";
  return true;
}

constexpr float kAuthoredGameAspect = 4.0f / 3.0f;

// The stable recomp module is generated from a DOL containing this hook. A
// savestate made before that build can restore the original zero-filled code
// cave while the statically compiled call site still branches into it. Repair
// the executable words while the CPU is stopped inside the post-load callback
// so static dispatch validation and interpreter fallback observe the same code.
constexpr std::array<std::pair<u32, u32>, 4> kColosseumWidescreenGuestHook = {{
    {0x80005300, 0xC3A2B084},
    {0x80005304, 0xEFBD00B2},
    {0x80005308, 0x4809E62C},
    {0x800A3930, 0x4BF619D0},
}};

// Set after every savestate load. States carry the game's L1 frame period, so a
// state saved in 60 FPS mode would keep running at 60 with the setting off; the
// per-field hook restores the game's 30 FPS period once after each load.
std::atomic<bool> s_state_loaded_frame_period{false};

void RestoreColosseumWidescreenGuestHook()
{
  s_state_loaded_frame_period.store(true, std::memory_order_release);
  Core::System& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  for (const auto& [address, instruction] : kColosseumWidescreenGuestHook)
    ApplyMemoryPatch<u32>(guard, instruction, address);

  // Older states can also restore a zero projection multiplier. The compiled
  // hook consumes this word before the first rendered frame, so seed the
  // authored 4:3 scale here and let the scene-aware frame hook widen eligible
  // field gameplay afterward.
  ApplyMemoryPatch<u32>(guard, std::bit_cast<u32>(1.0f), 0x8047E724);
}

// The PDA frame itself occupies 602 of the authored 640 horizontal pixels.
// Match that edge so the ultrawide extension covers only the unused world
// bands and never overlaps the PDA's foreground widgets.
constexpr float kPdaContentAspect = 602.0f / 480.0f;

struct PdaBackgroundLayout {
  int left_end = 0;
  int right_begin = 0;
  int scanline_spacing = 4;
  int scanline_thickness = 1;
};

PdaBackgroundLayout GetPdaBackgroundLayout(int width, int height);

void DrawAuthoredBackdropMargins(NavigationOverlayState& state)
{
  const auto backdrop =
      static_cast<moderngekko::automation::ColosseumAuthoredBackdrop>(
      state.authored_backdrop.load(std::memory_order_acquire));
  if (backdrop == moderngekko::automation::ColosseumAuthoredBackdrop::None)
    return;

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const int width = static_cast<int>(std::lround(display.x));
  const int height = static_cast<int>(std::lround(display.y));
  const int side_width =
      backdrop == moderngekko::automation::ColosseumAuthoredBackdrop::Naming ?
          moderngekko::automation::ResolveColosseumNamingBackdropSideWidth(width, height) :
          moderngekko::automation::ResolveColosseumAuthoredCanvasSideWidth(width, height);
  if (side_width < 1)
    return;

  ImDrawList* const overlay = ImGui::GetForegroundDrawList();
  const float left = static_cast<float>(side_width);
  const float right = display.x - left;
  // These bands imitate the guest's scrolling striped backdrop, so their colours
  // have to match whatever texture the guest is actually drawing. Sampled from
  // the rendered frame at 1920x1440 on the naming screen: stock art is flat and
  // exactly these values, while the community HD pack retextures the backdrop
  // darker and less green. Using the stock palette against the HD pack is what
  // produced a visible seam at the margin boundary.
  //
  // A third-party pack with a different palette will still mismatch; the
  // durable fix is sampling the presented frame, which needs a per-frame
  // readback this renderer has no cheap path for.
  const bool hd_backdrop = state.community_hd_texture_pack_enabled.load(
      std::memory_order_relaxed);
  const ImU32 dark =
      hd_backdrop ? IM_COL32(8, 66, 140, 255) : IM_COL32(0, 81, 148, 255);
  const ImU32 light =
      hd_backdrop ? IM_COL32(8, 97, 198, 255) : IM_COL32(0, 109, 198, 255);
  overlay->AddRectFilled({0.0f, 0.0f}, {left, display.y}, dark);
  overlay->AddRectFilled({right, 0.0f}, display, dark);

  if (backdrop == moderngekko::automation::ColosseumAuthoredBackdrop::Naming)
  {
    int light_start = -1;
    for (int y = 0; y <= height; ++y)
    {
      const bool light_band = y < height &&
                              moderngekko::automation::
                                  IsColosseumNamingBackdropLightBand(y, height);
      if (light_band && light_start < 0)
      {
        light_start = y;
      }
      else if (!light_band && light_start >= 0)
      {
        overlay->AddRectFilled({0.0f, static_cast<float>(light_start)},
                               {left, static_cast<float>(y)}, light);
        overlay->AddRectFilled({right, static_cast<float>(light_start)},
                               {display.x, static_cast<float>(y)}, light);
        light_start = -1;
      }
    }
    return;
  }

  const float band_height = std::max(display.y / 120.0f, 1.0f);
  for (float y = band_height; y < display.y; y += band_height * 2.0f)
  {
    const float end = std::min(y + band_height, display.y);
    overlay->AddRectFilled({0.0f, y}, {left, end}, light);
    overlay->AddRectFilled({right, y}, {display.x, end}, light);
  }
}

PdaBackgroundLayout GetPdaBackgroundLayout(int width, int height)
{
  const int content_width =
      std::min(width, static_cast<int>(std::lround(height * kPdaContentAspect)));
  const int side_width = std::max((width - content_width) / 2, 0);
  return {
      .left_end = side_width,
      .right_begin = width - side_width,
      .scanline_spacing = std::max(static_cast<int>(std::lround(height / 120.0f)), 4),
      .scanline_thickness = std::max(static_cast<int>(std::lround(height / 960.0f)), 1),
  };
}

void DrawInputOverlay(NavigationOverlayState& state)
{
  if (!state.input_overlay_enabled.load(std::memory_order_relaxed))
    return;

  GCPadStatus pad;
  {
    std::lock_guard lock(state.mutex);
    pad = state.input_status;
  }
  const float ui_scale =
      static_cast<float>(state.accessibility_ui_scale.load(
          std::memory_order_relaxed)) /
      100.0f;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({16.0f, display.y - 112.0f * ui_scale},
                          ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.78f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      {10.0f * ui_scale, 8.0f * ui_scale});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * ui_scale);
  if (ImGui::Begin("Controller Input###ModernGekkoInputOverlay", nullptr,
                   ImGuiWindowFlags_NoDecoration |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoSavedSettings))
  {
    ImGui::SetWindowFontScale(ui_scale);
    const auto button = [&](PadButton mask, const char* label,
                            const ImVec4& active_color) {
      const bool active = (pad.button & mask) != 0;
      ImGui::TextColored(active ? active_color :
                                  ImVec4(0.42f, 0.46f, 0.5f, 1.0f),
                         "%s", label);
      ImGui::SameLine();
    };
    button(PAD_BUTTON_A, "A", {0.25f, 1.0f, 0.35f, 1.0f});
    button(PAD_BUTTON_B, "B", {1.0f, 0.25f, 0.25f, 1.0f});
    button(PAD_BUTTON_X, "X", {0.35f, 0.75f, 1.0f, 1.0f});
    button(PAD_BUTTON_Y, "Y", {1.0f, 0.85f, 0.2f, 1.0f});
    button(PAD_TRIGGER_L, "L", {0.95f, 0.95f, 1.0f, 1.0f});
    button(PAD_TRIGGER_R, "R", {0.95f, 0.95f, 1.0f, 1.0f});
    button(PAD_TRIGGER_Z, "Z", {0.65f, 0.45f, 1.0f, 1.0f});
    ImGui::TextColored((pad.button & PAD_BUTTON_START) != 0 ?
                           ImVec4(1.0f, 1.0f, 1.0f, 1.0f) :
                           ImVec4(0.42f, 0.46f, 0.5f, 1.0f),
                       "START");
    const float stick_x =
        (static_cast<float>(pad.stickX) - GCPadStatus::MAIN_STICK_CENTER_X) /
        GCPadStatus::MAIN_STICK_RADIUS;
    const float stick_y =
        (static_cast<float>(pad.stickY) - GCPadStatus::MAIN_STICK_CENTER_Y) /
        GCPadStatus::MAIN_STICK_RADIUS;
    ImGui::Text("Stick %+0.2f  %+0.2f    Triggers %3u  %3u", stick_x,
                stick_y, pad.triggerLeft, pad.triggerRight);
    if (state.fast_forward_active.load(std::memory_order_relaxed))
    {
      ImGui::SameLine();
      ImGui::TextColored({0.3f, 1.0f, 0.9f, 1.0f}, "  FAST %dx",
                         state.fast_forward_multiplier.load(
                             std::memory_order_relaxed));
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}


void DrawNavigationOverlay()
{
  if (!ImGui::GetCurrentContext())
    return;
  moderngekko::pause_menu::Draw();
  if (Host_IsPauseMenuOpen())
    return;
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  DrawAuthoredBackdropMargins(*state);
  DrawInputOverlay(*state);

  if (state->pda_active.load(std::memory_order_acquire))
  {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const PdaBackgroundLayout layout = GetPdaBackgroundLayout(
        static_cast<int>(std::lround(display.x)),
        static_cast<int>(std::lround(display.y)));
    if (layout.left_end >= 1)
    {
      ImDrawList* const overlay = ImGui::GetForegroundDrawList();
      const ImU32 background = IM_COL32(3, 20, 14, 255);
      const ImU32 scanline = IM_COL32(1, 74, 7, 255);
      overlay->AddRectFilled({0.0f, 0.0f},
                             {static_cast<float>(layout.left_end), display.y},
                             background);
      overlay->AddRectFilled({static_cast<float>(layout.right_begin), 0.0f},
                             display, background);

      for (float y = 0.0f; y < display.y; y += layout.scanline_spacing)
      {
        const float line_end =
            std::min(y + static_cast<float>(layout.scanline_thickness), display.y);
        overlay->AddRectFilled({0.0f, y},
                               {static_cast<float>(layout.left_end), line_end},
                               scanline);
        overlay->AddRectFilled({static_cast<float>(layout.right_begin), y},
                               {display.x, line_end}, scanline);
      }
    }
  }

  const int mode = state->mode.load(std::memory_order_relaxed);
  if (mode == 0 ||
      !state->overlay_visible.load(std::memory_order_relaxed) ||
      !state->valid.load(std::memory_order_acquire))
  {
    return;
  }

  const float player_x = state->player_x.load(std::memory_order_relaxed);
  const float player_z = state->player_z.load(std::memory_order_relaxed);
  const float facing = state->player_facing.load(std::memory_order_relaxed);
  const float companion_x =
      state->companion_x.load(std::memory_order_relaxed);
  const float companion_z =
      state->companion_z.load(std::memory_order_relaxed);
  const bool companion_valid =
      state->companion_valid.load(std::memory_order_relaxed);
  const unsigned int room_id =
      state->room_id.load(std::memory_order_relaxed);
  const int render_surface_width =
      state->render_surface_width.load(std::memory_order_relaxed);
  const int render_surface_height =
      state->render_surface_height.load(std::memory_order_relaxed);
  const float render_surface_aspect =
      state->render_surface_aspect.load(std::memory_order_relaxed);
  const unsigned int render_efb_scale =
      state->render_efb_scale.load(std::memory_order_relaxed);
  std::vector<NavigationActor> npcs;
  std::vector<NavigationSegment> collision_segments;
  bool collision_valid = false;
  std::string location_name;
  std::string ui_status;
  float collision_min_x = 0.0f;
  float collision_max_x = 0.0f;
  float collision_min_z = 0.0f;
  float collision_max_z = 0.0f;
  {
    std::lock_guard lock(state->mutex);
    npcs = state->npcs;
    collision_segments = state->collision_segments;
    collision_valid = state->collision_valid;
    location_name = state->location_name;
    ui_status = state->ui_status;
    collision_min_x = state->collision_min_x;
    collision_max_x = state->collision_max_x;
    collision_min_z = state->collision_min_z;
    collision_max_z = state->collision_max_z;
  }

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float ui_scale =
      static_cast<float>(state->accessibility_ui_scale.load(
          std::memory_order_relaxed)) /
      100.0f;
  const bool high_contrast =
      state->minimap_high_contrast.load(std::memory_order_relaxed);
  const float world_width =
      collision_valid ? std::max(collision_max_x - collision_min_x, 1.0f) : 1.0f;
  const float world_depth =
      collision_valid ? std::max(collision_max_z - collision_min_z, 1.0f) : 1.0f;
  const float world_aspect = std::clamp(world_width / world_depth, 0.42f, 1.35f);
  const float fitted_map_height =
      std::clamp((display.y - 150.0f) * ui_scale, 280.0f, 760.0f);
  const float fitted_map_width =
      std::clamp(fitted_map_height * world_aspect, 250.0f, 560.0f);
  const ImVec2 fitted_window_size{fitted_map_width + 18.0f,
                                  fitted_map_height + 72.0f};

  static NavigationOverlayState* previous_state = nullptr;
  static unsigned int previous_room = std::numeric_limits<unsigned int>::max();
  static bool fit_requested = true;
  if (previous_state != state)
  {
    previous_state = state;
    previous_room = std::numeric_limits<unsigned int>::max();
    fit_requested = true;
  }
  if (state->fit_map_requested.exchange(false, std::memory_order_relaxed))
    fit_requested = true;
  if (collision_valid && previous_room != room_id)
  {
    previous_room = room_id;
    fit_requested = true;
  }

  static float previous_display_width = -1.0f;
  static float previous_display_height = -1.0f;
  const bool display_size_changed =
      std::abs(display.x - previous_display_width) > 1.0f ||
      std::abs(display.y - previous_display_height) > 1.0f;
  previous_display_width = display.x;
  previous_display_height = display.y;

  constexpr float margin = 18.0f;
  ImGui::SetNextWindowPos({display.x - fitted_window_size.x - margin, margin},
                          display_size_changed ? ImGuiCond_Always :
                                                 ImGuiCond_Appearing);
  if (fit_requested)
  {
    ImGui::SetNextWindowSize(fitted_window_size, ImGuiCond_Always);
    fit_requested = false;
  }
  ImGui::SetNextWindowSizeConstraints(
      {250.0f, 280.0f},
      {std::max(250.0f, display.x - margin * 2.0f),
       std::max(280.0f, display.y - margin * 2.0f)});

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(6, 13, 22, 26));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(8, 24, 34, 46));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(10, 38, 48, 56));
  ImGui::PushStyleColor(ImGuiCol_MenuBarBg, IM_COL32(5, 18, 27, 30));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(70, 220, 230, 210));
  ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(70, 220, 230, 170));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(105, 245, 250, 240));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, IM_COL32(255, 215, 60, 255));
  const std::string title =
      location_name.empty() ? fmt::format("Room {:02X}###Field Navigation", room_id) :
                              fmt::format("{}###Field Navigation", location_name);
  const bool window_visible =
      ImGui::Begin(title.c_str(), nullptr,
                   ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleColor(8);
  ImGui::PopStyleVar(2);
  if (!window_visible)
  {
    ImGui::End();
    return;
  }

  constexpr float map_padding = 5.0f;
  const float footer_height = mode >= 2 ? 25.0f : 5.0f;
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const ImVec2 map_min{ImGui::GetCursorScreenPos().x + map_padding,
                       ImGui::GetCursorScreenPos().y + map_padding};
  const ImVec2 map_max{
      map_min.x + std::max(1.0f, available.x - map_padding * 2.0f),
      map_min.y +
          std::max(1.0f, available.y - map_padding * 2.0f - footer_height)};
  const ImVec2 map_center{(map_min.x + map_max.x) * 0.5f,
                          (map_min.y + map_max.y) * 0.5f};
  ImGui::Dummy(available);
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(map_min, map_max,
                      high_contrast ? IM_COL32(0, 0, 0, 178) :
                                      IM_COL32(3, 10, 16, 26),
                      4.0f);

  float world_center_x = player_x;
  float world_center_z = player_z;
  float zoom = 0.45f;
  if (collision_valid)
  {
    world_center_x = (collision_min_x + collision_max_x) * 0.5f;
    world_center_z = (collision_min_z + collision_max_z) * 0.5f;
    zoom = std::min((map_max.x - map_min.x - 10.0f) / world_width,
                    (map_max.y - map_min.y - 10.0f) / world_depth);
    zoom = std::clamp(zoom, 0.03f, 8.0f);
  }
  const auto world_to_screen = [&](NavigationPoint point) {
    return ImVec2{map_center.x + (point.x - world_center_x) * zoom,
                  map_center.y + (point.z - world_center_z) * zoom};
  };

  draw->PushClipRect(map_min, map_max, true);
  constexpr float grid_world = 50.0f;
  const float world_radius_x = (map_max.x - map_min.x) * 0.5f / zoom;
  const float world_radius_z = (map_max.y - map_min.y) * 0.5f / zoom;
  const float first_x =
      std::floor((world_center_x - world_radius_x) / grid_world) * grid_world;
  const float first_z =
      std::floor((world_center_z - world_radius_z) / grid_world) * grid_world;
  for (float x = first_x; x <= world_center_x + world_radius_x; x += grid_world)
  {
    const float screen_x = map_center.x + (x - world_center_x) * zoom;
    draw->AddLine({screen_x, map_min.y}, {screen_x, map_max.y},
                  IM_COL32(70, 100, 115, 100));
  }
  for (float z = first_z; z <= world_center_z + world_radius_z; z += grid_world)
  {
    const float screen_y = map_center.y + (z - world_center_z) * zoom;
    draw->AddLine({map_min.x, screen_y}, {map_max.x, screen_y},
                  IM_COL32(70, 100, 115, 100));
  }

  for (const NavigationSegment& segment : collision_segments)
  {
    ImU32 color;
    float thickness;
    switch (segment.kind)
    {
    case NavigationSegmentKind::Walkable:
      color = high_contrast ? IM_COL32(80, 200, 255, 220) :
                              IM_COL32(45, 135, 145, 150);
      thickness = 1.0f;
      break;
    case NavigationSegmentKind::Trigger:
      color = high_contrast ? IM_COL32(255, 220, 0, 255) :
                              IM_COL32(255, 165, 65, 215);
      thickness = 1.5f;
      break;
    case NavigationSegmentKind::Wall:
    default:
      color = high_contrast ? IM_COL32(255, 255, 255, 255) :
                              IM_COL32(105, 245, 250, 240);
      thickness = 2.0f;
      break;
    }
    draw->AddLine(world_to_screen(segment.from), world_to_screen(segment.to),
                  color, thickness);
  }

  for (const NavigationActor& npc_actor : npcs)
  {
    const ImVec2 npc = world_to_screen(npc_actor.position);
    if (npc.x >= map_min.x && npc.x <= map_max.x &&
        npc.y >= map_min.y && npc.y <= map_max.y)
    {
      // Scripted actors are the ones that talk back, so keep them the bright
      // solid dot. Placed-but-idle actors are drawn hollow and dimmer: they are
      // still obstacles worth routing around, and they used to be invisible
      // here entirely, which made the map look emptier than the room was.
      if (npc_actor.scripted)
      {
        draw->AddCircleFilled(npc, 2.0f * ui_scale,
                              high_contrast ? IM_COL32(0, 255, 90, 255) :
                                              IM_COL32(150, 255, 105, 245));
        draw->AddCircle(npc, 3.0f * ui_scale, IM_COL32(235, 255, 220, 240), 0,
                        1.0f * ui_scale);
      }
      else
      {
        draw->AddCircle(npc, 2.5f * ui_scale,
                        high_contrast ? IM_COL32(255, 255, 255, 255) :
                                        IM_COL32(150, 255, 105, 170),
                        0, 1.0f * ui_scale);
      }
    }
  }


  const ImVec2 companion = world_to_screen({companion_x, companion_z});
  if (companion_valid && companion.x >= map_min.x && companion.x <= map_max.x &&
      companion.y >= map_min.y && companion.y <= map_max.y)
  {
    draw->AddCircleFilled(companion, 1.75f, IM_COL32(255, 110, 190, 240));
    draw->AddCircle(companion, 2.25f, IM_COL32(255, 230, 250, 255), 0,
                    1.5f);
  }

  const ImVec2 player = world_to_screen({player_x, player_z});
  const ImVec2 forward{std::sin(facing), std::cos(facing)};
  const ImVec2 right{forward.y, -forward.x};
  const ImVec2 tip{player.x + forward.x * 3.0f * ui_scale,
                   player.y + forward.y * 3.0f * ui_scale};
  const ImVec2 left{player.x - forward.x * 1.75f * ui_scale +
                                 right.x * 1.75f * ui_scale,
                    player.y - forward.y * 1.75f * ui_scale +
                                 right.y * 1.75f * ui_scale};
  const ImVec2 right_point{
      player.x - forward.x * 1.75f * ui_scale -
          right.x * 1.75f * ui_scale,
      player.y - forward.y * 1.75f * ui_scale -
          right.y * 1.75f * ui_scale};
  draw->AddTriangleFilled(tip, left, right_point,
                          IM_COL32(255, 215, 60, 255));
  draw->AddTriangle(tip, left, right_point, IM_COL32(255, 255, 230, 255),
                    1.25f);
  draw->PopClipRect();

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  if (mouse.x >= map_min.x && mouse.x <= map_max.x &&
      mouse.y >= map_min.y && mouse.y <= map_max.y)
  {
    constexpr float hover_radius = 16.0f;
    enum class HoveredActor
    {
      None,
      Wes,
      Rui,
      Npc
    };
    HoveredActor hovered_actor = HoveredActor::None;
    const NavigationActor* hovered_npc = nullptr;
    float closest_distance = hover_radius;
    const auto consider = [&](ImVec2 point, HoveredActor actor,
                              const NavigationActor* npc = nullptr) {
      const float distance = std::hypot(mouse.x - point.x, mouse.y - point.y);
      if (distance > closest_distance)
        return;
      closest_distance = distance;
      hovered_actor = actor;
      hovered_npc = npc;
    };
    consider(player, HoveredActor::Wes);
    if (companion_valid)
      consider(companion, HoveredActor::Rui);
    for (const NavigationActor& npc_actor : npcs)
      consider(world_to_screen(npc_actor.position), HoveredActor::Npc,
               &npc_actor);

    if (hovered_actor == HoveredActor::Wes)
    {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted("Wes");
      ImGui::EndTooltip();
    }
    else if (hovered_actor == HoveredActor::Rui)
    {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted("Rui");
      ImGui::EndTooltip();
    }
    else if (hovered_actor == HoveredActor::Npc && hovered_npc)
    {
      ImGui::BeginTooltip();
      ImGui::Text("NPC %u", hovered_npc->slot);
      ImGui::TextDisabled("Actor type 0x%02X  %s", hovered_npc->archetype,
                          hovered_npc->scripted ? "scripted" : "idle");
      ImGui::EndTooltip();
    }
  }

  if (mode >= 2)
  {
    const std::string coordinates =
        fmt::format("x {:+.1f}  z {:+.1f}  a {:.2f}", player_x, player_z,
                    facing);
    const std::string render_status =
        state->widescreen_enabled.load(std::memory_order_relaxed) &&
                render_surface_width > 0 && render_surface_height > 0
            ? fmt::format("{}x{}  {:.2f}:1  {}x EFB", render_surface_width,
                          render_surface_height, render_surface_aspect,
                          render_efb_scale)
            : std::string{};
    const float footer_y = map_max.y + 5.0f;
    draw->AddText({map_min.x, footer_y}, IM_COL32(230, 245, 250, 255),
                  coordinates.c_str());
    const std::string& right_status =
        !ui_status.empty() ? ui_status : render_status;
    if (!right_status.empty())
    {
      const ImVec2 status_size = ImGui::CalcTextSize(right_status.c_str());
      draw->AddText({map_max.x - status_size.x, footer_y},
                    IM_COL32(105, 245, 250, 255), right_status.c_str());
    }
  }
  ImGui::End();
}

struct RuntimeAutomationState
{
  mutable std::mutex mutex;
  std::atomic<std::uint64_t> frame_count{0};
  std::atomic<std::uint64_t> present_count{0};
  std::uint64_t processed_commands = 0;
  std::string last_command;
  std::string last_error;
  std::string last_screenshot;
  std::uint64_t watchdog_recoveries = 0;
  std::string watchdog_last_recovery;
};

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  const auto now = std::chrono::steady_clock::now();
  std::string formatted_title = fmt::format("{} | {:.1f} FPS", title, fps);
  const NetPlay::InputWaitTelemetry telemetry =
      NetPlay::NetPlayClient::GetInputWaitTelemetry();
  if (!telemetry.active) {
    s_previous_net_wait_ns = 0;
    s_net_wait_ms_per_second = 0.0;
    s_previous_net_wait_sample = {};
    return formatted_title;
  }
  if (s_previous_net_wait_sample.time_since_epoch().count() == 0) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  } else if (telemetry.total_wait_ns < s_previous_net_wait_ns) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
    s_net_wait_ms_per_second = 0.0;
  } else if (now - s_previous_net_wait_sample >=
             std::chrono::milliseconds(500)) {
    const double seconds =
        std::chrono::duration<double>(now - s_previous_net_wait_sample).count();
    s_net_wait_ms_per_second =
        static_cast<double>(telemetry.total_wait_ns - s_previous_net_wait_ns) /
        1000000.0 / seconds;
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  }
  return fmt::format("{} | Net wait {:.1f} ms/s | Buffer {}", formatted_title,
                     s_net_wait_ms_per_second, telemetry.buffer_size);
}
} // namespace

namespace moderngekko {
bool SetColosseum60FpsEnabled(const std::filesystem::path& ini_path,
                              bool enabled, std::string* status);
bool SetColosseumIdleSkipEnabled(const std::filesystem::path& ini_path,
                                 bool enabled, std::string* status);
}

std::vector<std::string> Host_GetNavigationSavestateNames()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return {};

  std::vector<std::string> names;
  for (const std::filesystem::path& path :
       ListNavigationSavestates(state->savestate_directory))
  {
    names.push_back(path.filename().string());
  }
  return names;
}

void Host_RequestNavigationStateSave()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const std::filesystem::path path =
      MakeNavigationSavestatePath(state->savestate_directory);
  std::lock_guard lock(state->mutex);
  state->pending_save_state = path;
  state->pending_save_is_recovery = false;
  state->ui_status = "State save queued";
}

void Host_RequestGameReset()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  std::lock_guard lock(state->mutex);
  state->pending_reset = true;
  state->ui_status = "Restart queued";
}

void Host_RequestPauseToggle()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  std::lock_guard lock(state->mutex);
  state->pending_pause_toggle = true;
}

void Host_RequestAudioMuteToggle()
{
  Core::QueueHostJob([](Core::System& system) {
    auto* state = s_navigation_overlay_state.load(std::memory_order_acquire);
    if (!state || !Core::IsRunning(system))
      return;
    const bool muted = !Config::Get(Config::MAIN_AUDIO_MUTED);
    Config::SetBase(Config::MAIN_AUDIO_MUTED, muted);
    Config::Save();
    AudioCommon::UpdateSoundStream(system);
    state->audio_muted.store(muted, std::memory_order_relaxed);
  });
}

void Host_TogglePauseMenuFullscreen()
{
  if (s_platform)
    s_platform->ToggleFullscreenFromMenu();
}

void Host_SetExclusiveFullscreen(bool exclusive)
{
  if (s_platform)
    s_platform->SetExclusiveFullscreen(exclusive);
}

bool Host_IsExclusiveFullscreen()
{
  return s_platform && s_platform->IsExclusiveFullscreenPreferred();
}

bool Host_IsGamePaused()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->paused.load(std::memory_order_relaxed);
}

bool Host_IsAudioMuted()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->audio_muted.load(std::memory_order_relaxed);
}

void Host_RequestNavigationStateLoad(const std::string& filename)
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state || std::filesystem::path(filename).filename() != filename)
    return;

  const std::filesystem::path path = state->savestate_directory / filename;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return;

  std::lock_guard lock(state->mutex);
  state->pending_load_state = path;
  state->ui_status = "State load queued";
}

bool Host_IsNavigationWidescreenEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state &&
         state->widescreen_enabled.load(std::memory_order_relaxed);
}

bool Host_IsNavigationOverlayVisible()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->mode.load(std::memory_order_relaxed) != 0 &&
         state->overlay_visible.load(std::memory_order_relaxed);
}

int Host_GetUltrawideEfbScale()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state ? state->ultrawide_efb_scale.load(std::memory_order_relaxed) : 5;
}

// Called on the CPU thread. Keep both scene modes at the selected scale so
// authored menus and door transitions cannot restore the previous resolution.
void Host_SetPauseMenuResolution(int scale)
{
  if (scale < 1 || scale > 6)
    return;
  auto* state = s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;
  state->standard_efb_scale = scale;
  state->ultrawide_efb_scale.store(scale, std::memory_order_relaxed);
  Config::SetCurrent(Config::GFX_EFB_SCALE, scale);
}

void Host_SetUltrawideEfbScale(int scale)
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state || (scale != 3 && scale != 4 && scale != 5 && scale != 6))
    return;

  state->ultrawide_efb_scale.store(scale, std::memory_order_relaxed);
  if (state->widescreen_enabled.load(std::memory_order_relaxed))
    Config::SetCurrent(Config::GFX_EFB_SCALE, scale);

  bool saved = false;
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile frontend_ini;
    if (frontend_ini.Load(state->frontend_config_path.string()))
    {
      frontend_ini.GetOrCreateSection("Video")
          ->Set("ultrawide_efb_scale", scale);
      saved = frontend_ini.Save(state->frontend_config_path.string());
    }
  }

  std::lock_guard lock(state->mutex);
  state->ui_status = fmt::format("Ultrawide render quality: {}x EFB", scale);
  if (!saved)
    state->ui_status += " (session only)";
}

bool Host_IsHighResolutionTexturesEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state &&
         state->hires_textures_enabled.load(std::memory_order_relaxed);
}

void Host_ToggleHighResolutionTextures()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool enabled =
      !state->hires_textures_enabled.load(std::memory_order_relaxed);
  Config::SetCurrent(Config::GFX_HIRES_TEXTURES, enabled);
  state->hires_textures_enabled.store(enabled, std::memory_order_relaxed);

  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_texture_cache)
      g_texture_cache->Invalidate();
  });

  bool saved = false;
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile frontend_ini;
    if (frontend_ini.Load(state->frontend_config_path.string()))
    {
      frontend_ini.GetOrCreateSection("Video")->Set("hires_textures", enabled);
      saved = frontend_ini.Save(state->frontend_config_path.string());
    }
  }

  std::lock_guard lock(state->mutex);
  state->ui_status =
      enabled ? "High-resolution textures enabled"
              : "High-resolution textures disabled";
  if (!saved)
    state->ui_status += " (session only)";
}

bool Host_IsTextureDumpingEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state &&
         state->dump_textures_enabled.load(std::memory_order_relaxed);
}

bool Host_IsTextUpscaleEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->text_upscale_enabled.load(std::memory_order_relaxed);
}

void Host_ToggleTextUpscale()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool enabled =
      !state->text_upscale_enabled.load(std::memory_order_relaxed);
  SetColosseumTextUpscaleEnabled(enabled);
  state->text_upscale_enabled.store(enabled, std::memory_order_relaxed);

  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_texture_cache)
      g_texture_cache->Invalidate();
  });

  bool saved = false;
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile frontend_ini;
    if (frontend_ini.Load(state->frontend_config_path.string()))
    {
      frontend_ini.GetOrCreateSection("Video")->Set("text_upscale", enabled);
      saved = frontend_ini.Save(state->frontend_config_path.string());
    }
  }

  std::lock_guard lock(state->mutex);
  state->ui_status =
      enabled ? "High-resolution text enabled" : "High-resolution text disabled";
  if (!saved)
    state->ui_status += " (session only)";
}

void Host_ToggleTextureDumping()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool enabled =
      !state->dump_textures_enabled.load(std::memory_order_relaxed);
  Config::SetCurrent(Config::GFX_DUMP_TEXTURES, enabled);
  state->dump_textures_enabled.store(enabled, std::memory_order_relaxed);
  std::lock_guard lock(state->mutex);
  state->ui_status =
      enabled ? "Texture dumping enabled" : "Texture dumping disabled";
}

void Host_ToggleNavigationOverlay()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool visible =
      !state->overlay_visible.load(std::memory_order_relaxed);
  const int previous_mode = state->mode.load(std::memory_order_relaxed);
  const int mode =
      moderngekko::automation::ResolveColosseumNavigationOverlayMode(
          previous_mode, visible);
  state->mode.store(mode, std::memory_order_relaxed);
  state->overlay_visible.store(visible, std::memory_order_relaxed);
  if (mode != previous_mode && !state->frontend_config_path.empty())
  {
    Common::IniFile ini;
    if (ini.Load(state->frontend_config_path.string()))
    {
      ini.GetOrCreateSection("Video")->Set("navigation_overlay", mode);
      ini.Save(state->frontend_config_path.string());
    }
  }
  std::lock_guard lock(state->mutex);
  state->ui_status =
      visible ? "Navigation map shown" : "Navigation map hidden";
}

bool Host_IsInputOverlayEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state &&
         state->input_overlay_enabled.load(std::memory_order_relaxed);
}

void Host_ToggleInputOverlay()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;
  const bool enabled =
      !state->input_overlay_enabled.load(std::memory_order_relaxed);
  state->input_overlay_enabled.store(enabled, std::memory_order_relaxed);
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile ini;
    if (ini.Load(state->frontend_config_path.string()))
    {
      ini.GetOrCreateSection("Video")->Set("input_overlay", enabled);
      ini.Save(state->frontend_config_path.string());
    }
  }
  std::lock_guard lock(state->mutex);
  state->ui_status = enabled ? "Input overlay shown" : "Input overlay hidden";
}

bool Host_IsMinimapHighContrastEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state &&
         state->minimap_high_contrast.load(std::memory_order_relaxed);
}

void Host_ToggleMinimapHighContrast()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;
  const bool enabled =
      !state->minimap_high_contrast.load(std::memory_order_relaxed);
  state->minimap_high_contrast.store(enabled, std::memory_order_relaxed);
  const int previous_mode = state->mode.load(std::memory_order_relaxed);
  const int mode =
      moderngekko::automation::ResolveColosseumNavigationOverlayMode(
          previous_mode, enabled);
  state->mode.store(mode, std::memory_order_relaxed);
  if (mode != previous_mode)
    state->overlay_visible.store(true, std::memory_order_relaxed);
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile ini;
    if (ini.Load(state->frontend_config_path.string()))
    {
      ini.GetOrCreateSection("Video")->Set("minimap_high_contrast", enabled);
      if (mode != previous_mode)
        ini.GetOrCreateSection("Video")->Set("navigation_overlay", mode);
      ini.Save(state->frontend_config_path.string());
    }
  }
  std::lock_guard lock(state->mutex);
  state->ui_status = enabled ? "High-contrast map enabled" :
                               "High-contrast map disabled";
}

bool Host_IsFastForwardEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->fast_forward_enabled.load(std::memory_order_relaxed);
}

bool Host_IsCommunityHdTexturePackEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->community_hd_texture_pack_enabled.load(std::memory_order_relaxed);
}

void Host_ToggleCommunityHdTexturePack()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool enabled =
      !state->community_hd_texture_pack_enabled.load(std::memory_order_relaxed);
  std::string error;
  if (!moderngekko::SetCommunityHdTexturePackEnabled(
          state->frontend_config_path.parent_path(), enabled, &error))
  {
    std::lock_guard lock(state->mutex);
    state->ui_status = "HD texture pack: " + error;
    return;
  }

  state->community_hd_texture_pack_enabled.store(enabled, std::memory_order_relaxed);
  if (enabled)
  {
    Config::SetCurrent(Config::GFX_HIRES_TEXTURES, true);
    state->hires_textures_enabled.store(true, std::memory_order_relaxed);
  }
  HiresTexture::Update();
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_texture_cache)
      g_texture_cache->Invalidate();
  });

  bool saved = false;
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile frontend_ini;
    if (frontend_ini.Load(state->frontend_config_path.string()))
    {
      auto* const video = frontend_ini.GetOrCreateSection("Video");
      video->Set("community_hd_texture_pack", enabled);
      if (enabled)
        video->Set("hires_textures", true);
      saved = frontend_ini.Save(state->frontend_config_path.string());
    }
  }

  std::lock_guard lock(state->mutex);
  state->ui_status = enabled ?
                         "Community HD texture pack enabled (ModernGekko UI overrides active)" :
                         "Community HD texture pack disabled";
  if (!saved)
    state->ui_status += " (session only)";
}

bool Host_IsSixtyFpsEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->sixty_fps_enabled.load(std::memory_order_relaxed);
}

void Host_ToggleSixtyFps()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool enabled =
      !state->sixty_fps_enabled.load(std::memory_order_relaxed);
  std::string status;
  const std::filesystem::path game_ini =
      state->frontend_config_path.parent_path() / "GameSettings" / "GC6E01.ini";
  if (!moderngekko::SetColosseum60FpsEnabled(game_ini, enabled, &status))
  {
    std::lock_guard lock(state->mutex);
    state->ui_status = std::move(status);
    return;
  }

  state->sixty_fps_enabled.store(enabled, std::memory_order_relaxed);
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile ini;
    if (ini.Load(state->frontend_config_path.string()))
    {
      ini.GetOrCreateSection("QualityOfLife")->Set("sixty_fps", enabled);
      ini.Save(state->frontend_config_path.string());
    }
  }
  std::lock_guard lock(state->mutex);
  state->ui_status = std::move(status);
}

void Host_ToggleFastForwardEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;
  const bool enabled =
      !state->fast_forward_enabled.load(std::memory_order_relaxed);
  state->fast_forward_enabled.store(enabled, std::memory_order_relaxed);
  if (!enabled)
  {
    state->fast_forward_active.store(false, std::memory_order_relaxed);
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
  }
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile ini;
    if (ini.Load(state->frontend_config_path.string()))
    {
      ini.GetOrCreateSection("QualityOfLife")->Set("fast_forward", enabled);
      ini.Save(state->frontend_config_path.string());
    }
  }
  std::lock_guard lock(state->mutex);
  state->ui_status = enabled ? "Space fast-forward enabled" :
                               "Space fast-forward disabled";
}

void Host_SetFastForwardActive(bool active)
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state || !state->fast_forward_enabled.load(std::memory_order_relaxed))
    return;
  if (state->fast_forward_active.exchange(active, std::memory_order_relaxed) == active)
    return;
  const float speed = active ?
      static_cast<float>(state->fast_forward_multiplier.load(
          std::memory_order_relaxed)) :
      1.0f;
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, speed);
}

bool Host_IsAutosaveEnabled()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  return state && state->autosave_enabled.load(std::memory_order_relaxed);
}

void Host_ToggleAutosave()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;
  const bool enabled =
      !state->autosave_enabled.load(std::memory_order_relaxed);
  state->autosave_enabled.store(enabled, std::memory_order_relaxed);
  if (!state->frontend_config_path.empty())
  {
    Common::IniFile ini;
    if (ini.Load(state->frontend_config_path.string()))
    {
      ini.GetOrCreateSection("QualityOfLife")->Set("autosave", enabled);
      ini.Save(state->frontend_config_path.string());
    }
  }
  std::lock_guard lock(state->mutex);
  state->ui_status = enabled ? "Rotating autosaves enabled" :
                               "Rotating autosaves disabled";
}

void Host_ToggleNavigationWidescreen()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (!state)
    return;

  const bool enabled =
      !state->widescreen_enabled.load(std::memory_order_relaxed);
  std::string status;
  if (SetNavigationWidescreenEnabled(enabled, &status))
    state->widescreen_enabled.store(enabled, std::memory_order_relaxed);
  std::lock_guard lock(state->mutex);
  state->ui_status = std::move(status);
}

void Host_RequestNavigationMapFit()
{
  NavigationOverlayState* const state =
      s_navigation_overlay_state.load(std::memory_order_acquire);
  if (state)
    state->fit_map_requested.store(true, std::memory_order_relaxed);
}

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(
        title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  std::unique_ptr<ModManager> mods;
  Common::EventHook state_hook;
  Common::EventHook present_hook;
  Common::EventHook frame_hook;
  Common::EventHook navigation_hook;
  NavigationOverlayState navigation_overlay;
  RuntimeAutomationState automation_state;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  bool automation_registered = false;
  std::atomic<bool> running{false};
};

namespace
{
void EnsureAutomationDirectories(const std::filesystem::path& root)
{
  if (root.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(root / "commands", ec);
  std::filesystem::create_directories(root / "processed", ec);
  std::filesystem::create_directories(root / "failed", ec);
}

void SetAutomationError(RuntimeAutomationState& state, std::string message)
{
  std::lock_guard lock(state.mutex);
  state.last_error = std::move(message);
}

void UpdateNavigationOverlayState(NavigationOverlayState& state, bool valid, float player_x,
                                  float player_z, float facing, bool companion_valid,
                                  float companion_x, float companion_z, unsigned int room_id,
                                  std::span<const NavigationActor> npcs)
{
  state.player_x.store(player_x, std::memory_order_relaxed);
  state.player_z.store(player_z, std::memory_order_relaxed);
  state.player_facing.store(facing, std::memory_order_relaxed);
  state.companion_x.store(companion_x, std::memory_order_relaxed);
  state.companion_z.store(companion_z, std::memory_order_relaxed);
  state.companion_valid.store(companion_valid, std::memory_order_relaxed);
  state.room_id.store(room_id, std::memory_order_relaxed);
  state.valid.store(valid, std::memory_order_release);

  std::lock_guard lock(state.mutex);
  state.npcs.assign(npcs.begin(), npcs.end());
  if (!valid)
  {
    state.npcs.clear();
    return;
  }

  if (state.displayed_room_id != room_id)
  {
    if (state.has_seen_valid_room &&
        state.autosave_enabled.load(std::memory_order_relaxed) &&
        state.pending_save_state.empty() && state.pending_load_state.empty())
    {
      const int slots =
          state.autosave_slots.load(std::memory_order_relaxed);
      moderngekko::frontend::PruneAutomatic(
          state.savestate_directory,
          static_cast<std::size_t>(std::max(slots - 1, 0)), "recovery-room");
      state.pending_save_state =
          MakeRecoverySavestatePath(state.savestate_directory, room_id);
      state.pending_save_is_recovery = true;
      state.ui_status = "Recovery autosave queued";
    }
    state.collision_segments.clear();
    state.collision_valid = false;
    state.collision_room_id = std::numeric_limits<unsigned int>::max();
    state.collision_refresh_delay_frames = 30;
    state.collision_refresh_retries = 3;
    state.collision_base = 0;
    state.collision_archive_name.clear();
    state.location_name.clear();
    state.displayed_room_id = room_id;
  }
  state.has_seen_valid_room = true;
}

void MarkAutomationCommand(RuntimeAutomationState& state, std::string command_name)
{
  std::lock_guard lock(state.mutex);
  ++state.processed_commands;
  state.last_command = std::move(command_name);
  state.last_error.clear();
}

void ClearAutomationPad(int port)
{
  for (int control = static_cast<int>(ciface::Touch::FIRST_GC_CONTROL);
       control <= static_cast<int>(ciface::Touch::LAST_GC_CONTROL); ++control)
  {
    ciface::Touch::ClearControlState(port,
                                     static_cast<ciface::Touch::ControlID>(control));
  }
}

void ApplyAutomationPad(const automation::PadState& pad)
{
  ClearAutomationPad(pad.port);
  for (std::size_t index = 0; index < pad.controls.size(); ++index)
  {
    ciface::Touch::SetControlState(
        pad.port,
        static_cast<ciface::Touch::ControlID>(
            static_cast<int>(ciface::Touch::FIRST_GC_CONTROL) +
            static_cast<int>(index)),
        pad.controls[index]);
  }
}

std::optional<RuntimeError> ApplyAutomationPadForFrames(
    RuntimeAutomationState& state, const automation::Command& command,
    std::stop_token stop_token)
{
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) != Core::State::Running)
  {
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "frame-stepped input requires a running emulated core"};
  }
  std::uint64_t first_frame = state.frame_count.load(std::memory_order_relaxed);
  std::uint64_t last_frame = first_frame;
  auto last_progress = std::chrono::steady_clock::now();
  ApplyAutomationPad(command.pad);
  while (!stop_token.stop_requested())
  {
    const std::uint64_t current_frame =
        state.frame_count.load(std::memory_order_relaxed);
    if (current_frame < first_frame)
    {
      first_frame = current_frame;
      last_frame = current_frame;
      last_progress = std::chrono::steady_clock::now();
    }
    else if (current_frame - first_frame >= command.frames)
    {
      break;
    }
    else if (current_frame != last_frame)
    {
      last_frame = current_frame;
      last_progress = std::chrono::steady_clock::now();
    }
    else if (std::chrono::steady_clock::now() - last_progress >=
             std::chrono::seconds(10))
    {
      ClearAutomationPad(command.pad.port);
      return RuntimeError{RuntimeErrorCode::InvalidState,
                          "emulation produced no frames for 10 seconds"};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ClearAutomationPad(command.pad.port);
  return {};
}

std::filesystem::path NormalizeScreenshotPath(const std::filesystem::path& path)
{
  if (!path.has_extension())
    return path.string() + ".png";
  return path;
}

void SaveAutomationScreenshot(const std::filesystem::path& path)
{
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  if (g_frame_dumper)
    g_frame_dumper->SaveScreenshot(path.string());
}

std::optional<RuntimeError> ReadAutomationMemory(const std::filesystem::path& path, u32 address,
                                                 u32 size)
{
  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  const u8* source = system.GetMemory().GetPointerForRange(address, size);
  if (!source)
  {
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        fmt::format("guest range {:#010x}+{} is not readable", address, size)};
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
  {
    return RuntimeError{RuntimeErrorCode::InitializationFailed,
                        "could not create memory output directory: " + ec.message()};
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
  {
    return RuntimeError{RuntimeErrorCode::InitializationFailed,
                        "could not open memory output file"};
  }
  output.write(reinterpret_cast<const char*>(source), size);
  if (!output)
  {
    return RuntimeError{RuntimeErrorCode::InitializationFailed,
                        "could not write memory output file"};
  }
  return {};
}

std::optional<RuntimeError> WriteAutomationMemory(u32 address,
                                                  const std::vector<std::uint8_t>& data)
{
  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  if (!system.GetMemory().GetPointerForRange(address, data.size()))
  {
    return RuntimeError{
        RuntimeErrorCode::InvalidState,
        fmt::format("guest range {:#010x}+{} is not writable", address, data.size())};
  }
  system.GetMemory().CopyToEmu(address, data.data(), data.size());
  return {};
}

std::optional<RuntimeError> ApplyAutomationCommand(Runtime& runtime,
                                                   RuntimeAutomationState& state,
                                                   const automation::Command& command,
                                                   std::stop_token stop_token)
{
  auto& system = Core::System::GetInstance();
  switch (command.type)
  {
  case automation::CommandType::Pad:
    ApplyAutomationPad(command.pad);
    break;
  case automation::CommandType::PadFrames:
    if (auto error = ApplyAutomationPadForFrames(state, command, stop_token))
      return error;
    break;
  case automation::CommandType::ClearPad:
    ClearAutomationPad(command.pad.port);
    break;
  case automation::CommandType::Pause:
    if (auto error = runtime.Pause())
      return error;
    break;
  case automation::CommandType::Resume:
    if (auto error = runtime.Resume())
      return error;
    break;
  case automation::CommandType::SaveState:
    std::filesystem::create_directories(command.path.parent_path());
    State::SaveAs(system, command.path.string());
    break;
  case automation::CommandType::LoadState:
    State::LoadAs(system, command.path.string());
    break;
  case automation::CommandType::Screenshot:
    SaveAutomationScreenshot(NormalizeScreenshotPath(command.path));
    break;
  case automation::CommandType::ReadMemory:
    if (auto error = ReadAutomationMemory(command.path, command.address, command.size))
      return error;
    break;
  case automation::CommandType::WriteMemory:
    if (auto error = WriteAutomationMemory(command.address, command.data))
      return error;
    break;
  case automation::CommandType::Stop:
    runtime.RequestStop();
    break;
  }
  MarkAutomationCommand(state, command.source_name);
  return {};
}

bool AutomationCommandNeedsReadyCore(automation::CommandType type)
{
  switch (type)
  {
  case automation::CommandType::Pause:
  case automation::CommandType::Resume:
  case automation::CommandType::SaveState:
  case automation::CommandType::LoadState:
  case automation::CommandType::Screenshot:
  case automation::CommandType::ReadMemory:
  case automation::CommandType::WriteMemory:
  case automation::CommandType::PadFrames:
    return true;
  case automation::CommandType::Pad:
  case automation::CommandType::ClearPad:
  case automation::CommandType::Stop:
    return false;
  }
  return true;
}

bool AutomationCoreIsReady()
{
  const auto state = Core::GetState(Core::System::GetInstance());
  return state == Core::State::Running || state == Core::State::Paused;
}

void MoveAutomationCommand(const std::filesystem::path& source, const std::filesystem::path& root,
                           const char* destination_directory)
{
  std::error_code ec;
  const std::filesystem::path destination =
      root / destination_directory / source.filename();
  std::filesystem::remove(destination, ec);
  ec.clear();
  std::filesystem::rename(source, destination, ec);
  if (ec)
  {
    ec.clear();
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec)
      std::filesystem::remove(source, ec);
  }
}


template <typename ImplT>
void WriteAutomationStatus(const std::filesystem::path& root, ImplT& impl)
{
  automation::Status status;
  const auto core_state = impl.booted ? Core::GetState(Core::System::GetInstance())
                                      : Core::State::Uninitialized;
  status.state = core_state == Core::State::Paused
                     ? "paused"
                     : (core_state == Core::State::Running ? "running" : "stopped");
  status.booted = impl.booted;
  status.title = impl.title;
  status.game_id = impl.metadata.disc_id;
  status.game_name = impl.metadata.game_name;
  status.text_upscale_enabled = IsColosseumTextUpscaleEnabled();
  status.text_upscale_count = GetColosseumTextUpscaleCount();
  status.navigation_mode =
      impl.navigation_overlay.mode.load(std::memory_order_relaxed);
  status.navigation_valid =
      impl.navigation_overlay.valid.load(std::memory_order_acquire);
  status.movie_active =
      impl.navigation_overlay.movie_active.load(std::memory_order_acquire);
  status.live_title_scene_active =
      impl.navigation_overlay.live_title_scene_active.load(std::memory_order_acquire);
  status.authored_menu_active =
      impl.navigation_overlay.authored_menu_active.load(std::memory_order_acquire);
  status.menu_edge_fill_active =
      impl.navigation_overlay.menu_edge_fill_active.load(std::memory_order_relaxed);
  status.naming_presentation_active =
      impl.navigation_overlay.published_naming_presentation.load(std::memory_order_acquire);
  status.gameplay_ultrawide =
      impl.navigation_overlay.published_gameplay_ultrawide.load(std::memory_order_acquire);
  status.colosseum_message_id =
      impl.navigation_overlay.active_ui_message_id.load(std::memory_order_relaxed);
  status.colosseum_menu_active =
      impl.navigation_overlay.published_menu_active.load(std::memory_order_relaxed);
  status.aspect_mode =
      impl.navigation_overlay.published_aspect_mode.load(std::memory_order_acquire);
  status.presentation_transitions =
      impl.navigation_overlay.presentation_transitions.load(std::memory_order_relaxed);
  status.navigation_room_id =
      impl.navigation_overlay.room_id.load(std::memory_order_relaxed);
  status.navigation_player_x =
      impl.navigation_overlay.player_x.load(std::memory_order_relaxed);
  status.navigation_player_z =
      impl.navigation_overlay.player_z.load(std::memory_order_relaxed);
  status.navigation_facing =
      impl.navigation_overlay.player_facing.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(impl.navigation_overlay.mutex);
    status.navigation_collision_valid = impl.navigation_overlay.collision_valid;
    status.navigation_collision_base = impl.navigation_overlay.collision_base;
    status.navigation_collision_segments =
        static_cast<std::uint32_t>(impl.navigation_overlay.collision_segments.size());
    status.navigation_npc_count =
        static_cast<std::uint32_t>(impl.navigation_overlay.npcs.size());
    status.navigation_archive = impl.navigation_overlay.collision_archive_name;
    status.navigation_location = impl.navigation_overlay.location_name;
  }
  if (impl.booted)
  {
    const auto& perf = Core::System::GetInstance().GetPerfMetrics();
    status.fps = perf.GetFPS();
    status.vps = perf.GetVPS();
    status.speed = perf.GetSpeed();
  }
  status.frame_count =
      impl.automation_state.frame_count.load(std::memory_order_relaxed);
  status.present_count =
      impl.automation_state.present_count.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(impl.automation_state.mutex);
    status.processed_commands = impl.automation_state.processed_commands;
    status.last_command = impl.automation_state.last_command;
    status.last_error = impl.automation_state.last_error;
  }

  const std::filesystem::path status_path = root / "status.txt";
  const std::filesystem::path temp_path = root / "status.tmp";
  std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
  output << automation::FormatStatus(status);
  output.close();
  std::error_code ec;
  std::filesystem::rename(temp_path, status_path, ec);
  if (ec)
  {
    ec.clear();
    std::filesystem::copy_file(temp_path, status_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(temp_path, ec);
  }
}

constexpr std::string_view COLOSSEUM_60_FPS_NAME = "$60 FPS (Nerdzilla)";
constexpr std::string_view COLOSSEUM_60_FPS_PATCH = "04005D98 38600000";

// The task-list-aware guest idle skip is inert unless these three keys name
// the loop, so the toggle is expressed by writing them or zeroing them. They are
// game-specific and were confirmed three ways -- the decompilation, the
// generated chunk, and a runtime trace of r13 -- so the runtime owns the values
// rather than trusting whatever a stale ini happens to carry.
constexpr std::string_view COLOSSEUM_TASK_IDLE_PC = "0x800FEB9C";
constexpr std::string_view COLOSSEUM_TASK_LIST_HEAD = "0x8047AC98";
constexpr std::string_view COLOSSEUM_TASK_PENDING_HEAD = "0x8047AC9C";

bool SetColosseumIdleSkipIniEnabled(const std::filesystem::path& ini_path,
                                    bool enabled, std::string* status)
{
  Common::IniFile ini;
  if (std::filesystem::exists(ini_path) && !ini.Load(ini_path.string()))
  {
    if (status)
      *status = "Could not read the Pokemon Colosseum game settings";
    return false;
  }

  // Zero rather than delete when off: zero is the setting's own default, so the
  // feature is off either way, and a key that stays put is easier to see than
  // one that vanishes.
  Common::IniFile::Section* const core = ini.GetOrCreateSection("Core");
  core->Set("StaticRecompTaskIdlePC",
            std::string(enabled ? COLOSSEUM_TASK_IDLE_PC : "0x0"));
  core->Set("StaticRecompTaskListHead",
            std::string(enabled ? COLOSSEUM_TASK_LIST_HEAD : "0x0"));
  core->Set("StaticRecompTaskPendingHead",
            std::string(enabled ? COLOSSEUM_TASK_PENDING_HEAD : "0x0"));

  std::error_code directory_error;
  std::filesystem::create_directories(ini_path.parent_path(), directory_error);
  if (directory_error || !ini.Save(ini_path.string()))
  {
    if (status)
      *status = "Could not save the Pokemon Colosseum game settings";
    return false;
  }
  return true;
}

bool SetColosseum60FpsIniEnabled(const std::filesystem::path& ini_path,
                                 bool enabled, std::string* status)
{
  Common::IniFile ini;
  if (std::filesystem::exists(ini_path) && !ini.Load(ini_path.string()))
  {
    if (status)
      *status = "Could not read the Pokemon Colosseum game settings";
    return false;
  }

  std::vector<std::string> definitions;
  ini.GetLines("ActionReplay", &definitions, false);
  std::vector<std::string> filtered_definitions;
  bool skipping_existing_definition = false;
  for (const std::string& line : definitions)
  {
    if (line == COLOSSEUM_60_FPS_NAME)
    {
      skipping_existing_definition = true;
      continue;
    }
    if (skipping_existing_definition && !line.starts_with('$'))
      continue;
    skipping_existing_definition = false;
    filtered_definitions.push_back(line);
  }
  if (!filtered_definitions.empty() && !filtered_definitions.back().empty())
    filtered_definitions.emplace_back();
  filtered_definitions.emplace_back(COLOSSEUM_60_FPS_NAME);
  filtered_definitions.emplace_back(COLOSSEUM_60_FPS_PATCH);
  ini.SetLines("ActionReplay", std::move(filtered_definitions));

  const auto update_selection = [&](std::string_view section,
                                    bool include_code) {
    std::vector<std::string> lines;
    ini.GetLines(section, &lines, false);
    std::erase(lines, COLOSSEUM_60_FPS_NAME);
    if (include_code)
      lines.emplace_back(COLOSSEUM_60_FPS_NAME);
    ini.SetLines(section, std::move(lines));
  };
  update_selection("ActionReplay_Enabled", enabled);
  update_selection("ActionReplay_Disabled", false);

  std::error_code directory_error;
  std::filesystem::create_directories(ini_path.parent_path(), directory_error);
  if (directory_error || !ini.Save(ini_path.string()))
  {
    if (status)
      *status = "Could not save the Pokemon Colosseum game settings";
    return false;
  }
  // The Action Replay entry written here is inert under static recompilation:
  // guest instructions are translated ahead of time, so a runtime memory patch
  // cannot change what executes. 60 FPS is delivered by holding the engine's
  // frame period at 1 from the host each field, which takes effect immediately
  // and needs no relaunch.
  if (status)
    *status = enabled ? "60 FPS enabled" : "30 FPS restored";
  return true;
}

template <typename ImplT>
void AutomationLoop(Runtime& runtime, ImplT& impl, std::stop_token stop_token)
{
  const std::filesystem::path root = impl.config.automation.directory;
  const bool external_commands = !root.empty();
  if (external_commands)
    EnsureAutomationDirectories(root);
  auto next_status_write = std::chrono::steady_clock::now();
  auto watchdog_last_frame_time = std::chrono::steady_clock::now();
  std::uint64_t watchdog_last_frame =
      impl.automation_state.frame_count.load(std::memory_order_relaxed);
  while (!stop_token.stop_requested())
  {
    if (AutomationCoreIsReady())
    {
      std::filesystem::path save_path;
      std::filesystem::path load_path;
      bool save_is_recovery = false;
      bool reset_requested = false;
      bool pause_requested = false;
      bool mute_requested = false;
      {
        std::lock_guard lock(impl.navigation_overlay.mutex);
        save_path = std::exchange(impl.navigation_overlay.pending_save_state, {});
        save_is_recovery =
            std::exchange(impl.navigation_overlay.pending_save_is_recovery, false);
        load_path = std::exchange(impl.navigation_overlay.pending_load_state, {});
        reset_requested = std::exchange(impl.navigation_overlay.pending_reset, false);
        pause_requested = std::exchange(impl.navigation_overlay.pending_pause_toggle, false);
        mute_requested = std::exchange(impl.navigation_overlay.pending_mute_toggle, false);
      }
      if (pause_requested)
      {
        // Toggle rather than separate menu entries: one item that reads
        // "Pause" or "Resume" is less to get wrong than two that can disagree
        // with the core's actual state.
        const bool was_paused =
            impl.navigation_overlay.paused.load(std::memory_order_relaxed);
        const auto error = was_paused ? runtime.Resume() : runtime.Pause();
        if (!error)
        {
          impl.navigation_overlay.paused.store(!was_paused,
                                               std::memory_order_relaxed);
          std::lock_guard lock(impl.navigation_overlay.mutex);
          impl.navigation_overlay.ui_status = was_paused ? "Resumed" : "Paused";
        }
      }
      if (mute_requested)
      {
        // Dolphin keeps mute in config and the sound stream reads it on
        // update, so set both rather than stopping the stream: stopping it
        // loses the mixer state and clicks on resume.
        const bool muted =
            !impl.navigation_overlay.audio_muted.load(std::memory_order_relaxed);
        Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, muted);
        AudioCommon::UpdateSoundStream(Core::System::GetInstance());
        impl.navigation_overlay.audio_muted.store(muted, std::memory_order_relaxed);
        std::lock_guard lock(impl.navigation_overlay.mutex);
        impl.navigation_overlay.ui_status = muted ? "Audio muted" : "Audio unmuted";
      }
      if (!save_path.empty())
      {
        State::SaveAs(Core::System::GetInstance(), save_path.string());
        if (save_is_recovery)
        {
          std::ofstream recovery_pointer(
              impl.navigation_overlay.savestate_directory /
                  "recovery-latest.txt",
              std::ios::trunc);
          recovery_pointer << save_path.filename().string() << '\n';
        }
      }
      if (!load_path.empty())
      {
        if (impl.metadata.disc_id == "GC6E01" &&
            impl.navigation_overlay.widescreen_enabled.load(
                std::memory_order_relaxed))
        {
          impl.navigation_overlay.ultrawide_gameplay_started.store(
              true, std::memory_order_relaxed);
        }
        State::LoadAs(Core::System::GetInstance(), load_path.string());
      }
      if (reset_requested)
      {
        // A soft reset -- what the console's RESET button does -- rather than
        // tearing down and re-booting the core. It returns the game to its
        // title sequence without disturbing the recompiled module, the
        // savestate directory or anything else the session holds open.
        Core::System& system = Core::System::GetInstance();
        Core::RunOnCPUThread(system, [&system] {
          system.GetProcessorInterface().ResetButton_Tap();
        });
      }
    }

    const auto watchdog_now = std::chrono::steady_clock::now();
    const std::uint64_t watchdog_current_frame =
        impl.automation_state.frame_count.load(std::memory_order_relaxed);
    if (watchdog_current_frame != watchdog_last_frame)
    {
      watchdog_last_frame = watchdog_current_frame;
      watchdog_last_frame_time = watchdog_now;
    }
    else if (impl.config.qol.crash_watchdog && impl.booted &&
             Core::GetState(Core::System::GetInstance()) == Core::State::Running &&
             watchdog_now - watchdog_last_frame_time >=
                 std::chrono::seconds(impl.config.qol.crash_watchdog_seconds))
    {
      const auto recovery = moderngekko::frontend::LatestAutomatic(
          impl.navigation_overlay.savestate_directory, "recovery-room");
      if (recovery)
      {
        if (impl.metadata.disc_id == "GC6E01" &&
            impl.navigation_overlay.widescreen_enabled.load(
                std::memory_order_relaxed))
        {
          impl.navigation_overlay.ultrawide_gameplay_started.store(
              true, std::memory_order_relaxed);
        }
        State::LoadAs(Core::System::GetInstance(), recovery->string());
        std::lock_guard lock(impl.automation_state.mutex);
        ++impl.automation_state.watchdog_recoveries;
        impl.automation_state.watchdog_last_recovery =
            recovery->filename().string();
        impl.automation_state.last_error = fmt::format(
            "watchdog restored the latest recovery state after a {}-second frame stall",
            impl.config.qol.crash_watchdog_seconds);
      }
      else
      {
        SetAutomationError(
            impl.automation_state,
            fmt::format(
                "watchdog detected a {}-second frame stall but no recovery state exists",
                impl.config.qol.crash_watchdog_seconds));
      }
      watchdog_last_frame_time = watchdog_now;
    }

    if (external_commands)
    for (const auto& command_path : automation::ListCommandFiles(root / "commands"))
    {
      automation::Command command;
      std::string error;
      if (!automation::ParseCommandFile(command_path, &command, &error))
      {
        SetAutomationError(impl.automation_state,
                           command_path.filename().string() + ": " + error);
        MoveAutomationCommand(command_path, root, "failed");
        continue;
      }

      if (AutomationCommandNeedsReadyCore(command.type) && !AutomationCoreIsReady())
        break;

      if (!command.path.empty())
        command.path = automation::ResolveControlPath(root, command.path);
      if (auto runtime_error =
              ApplyAutomationCommand(runtime, impl.automation_state, command, stop_token))
      {
        SetAutomationError(impl.automation_state,
                           command.source_name + ": " + runtime_error->message);
        MoveAutomationCommand(command_path, root, "failed");
        continue;
      }

      MoveAutomationCommand(command_path, root, "processed");
    }

    const auto now = std::chrono::steady_clock::now();
    if (external_commands && now >= next_status_write)
    {
      WriteAutomationStatus(root, impl);
      next_status_write = now + std::chrono::milliseconds(200);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (external_commands)
    WriteAutomationStatus(root, impl);
}
}  // namespace

bool SetColosseum60FpsEnabled(const std::filesystem::path& ini_path,
                              bool enabled, std::string* status)
{
  return SetColosseum60FpsIniEnabled(ini_path, enabled, status);
}

bool SetColosseumIdleSkipEnabled(const std::filesystem::path& ini_path,
                                 bool enabled, std::string* status)
{
  return SetColosseumIdleSkipIniEnabled(ini_path, enabled, status);
}

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      "ModernGekko - " + impl->metadata.game_name + " [" +
      impl->metadata.disc_id + "]");
  impl->mods = std::make_unique<ModManager>();
  const ModLoadReport mod_report = impl->mods->LoadDirectories(
      impl->config.mod_directories, impl->metadata.disc_id);
  for (const ModLoadIssue &issue : mod_report.issues)
    std::fprintf(stderr, "mod rejected: %s: %s\n", issue.source.c_str(),
                 issue.message.c_str());
  for (const LoadedModInfo &mod : mod_report.loaded)
    std::fprintf(stderr, "mod loaded: %s %s\n", mod.id.c_str(),
                 mod.version.c_str());
  for (const LoadedModInfo &mod : mod_report.loaded) {
    impl->config.graphics.native_ultrawide |=
        impl->mods->FindExport(mod.id,
                               "moderngekko.feature.native_ultrawide") != nullptr;
    impl->config.graphics.text_upscale |=
        impl->mods->FindExport(mod.id,
                               "moderngekko.feature.text_upscale") != nullptr;
    impl->config.graphics.minimap_high_contrast |=
        impl->mods->FindExport(
            mod.id, "moderngekko.feature.minimap_high_contrast") != nullptr;
  }
  impl->config.graphics.navigation_overlay =
      automation::ResolveColosseumNavigationOverlayMode(
          impl->config.graphics.navigation_overlay,
          impl->config.graphics.minimap_high_contrast);

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(impl->config.user_directory.string());
    UICommon::Init();
    impl->ui_initialized = true;
  }
  std::string community_pack_error;
  if (!moderngekko::SetCommunityHdTexturePackEnabled(
          impl->config.user_directory,
          impl->config.graphics.community_hd_texture_pack,
          &community_pack_error))
  {
    std::fprintf(stderr, "community HD texture pack unavailable: %s\n",
                 community_pack_error.c_str());
    impl->config.graphics.community_hd_texture_pack = false;
  }
  if (impl->config.graphics.community_hd_texture_pack)
    impl->config.graphics.hires_textures = true;
  // MODERNGEKKO_START_FULLSCREEN=1: set by the pause menu's Graphics API switch
  // so the relaunched session keeps fullscreen.
  const char* start_fullscreen = std::getenv("MODERNGEKKO_START_FULLSCREEN");
  Config::SetBase(Config::MAIN_FULLSCREEN,
                  impl->config.fullscreen || (start_fullscreen && start_fullscreen[0] == '1'));

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE,
                    *impl->config.graphics.internal_resolution_scale);
  Config::SetBase(Config::GFX_WIDESCREEN_HACK,
                  impl->config.graphics.widescreen_hack);
  Config::SetBase(Config::GFX_ASPECT_RATIO,
                  static_cast<AspectMode>(impl->config.graphics.aspect_ratio));
  Config::SetBase(
      Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
      static_cast<TextureFilteringMode>(impl->config.graphics.texture_filtering));
  Config::SetBase(
      Config::GFX_ENHANCE_MAX_ANISOTROPY,
      static_cast<AnisotropicFilteringMode>(
          impl->config.graphics.max_anisotropy));
  Config::SetBase(Config::MAIN_OSD_FONT_SIZE,
                  impl->config.graphics.osd_font_size);
  Config::SetBase(Config::GFX_HIRES_TEXTURES,
                  impl->config.graphics.hires_textures);
  Config::SetBase(Config::GFX_CACHE_HIRES_TEXTURES,
                  impl->config.graphics.cache_hires_textures);
  Config::SetBase(Config::GFX_DUMP_TEXTURES,
                  impl->config.graphics.dump_textures);
  SetColosseumTextUpscaleEnabled(impl->config.graphics.text_upscale);
  Config::SetBase(Config::GFX_SHADER_CACHE, true);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (impl->config.headless) {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  } else if (impl->config.audio.backend.empty() ||
             !std::ranges::contains(audio_backends,
                                    impl->config.audio.backend)) {
    constexpr std::array preferred_backends = {
        BACKEND_CUBEB, BACKEND_PULSEAUDIO, BACKEND_ALSA};
    const auto preferred =
        std::ranges::find_if(preferred_backends, [&](const char *backend) {
          return std::ranges::contains(audio_backends, backend);
        });
    impl->config.audio.backend =
        preferred != preferred_backends.end() ? *preferred : BACKEND_NULLSOUND;
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                  impl->config.input.background_input);
  EnsureAutomationDirectories(impl->config.automation.directory);
  if (!impl->config.automation.directory.empty()) {
    for (int port = 0; port < 4; ++port)
      ciface::Touch::RegisterGameCubeInputOverrider(port);
    impl->automation_registered = true;
  }

  auto &jit = Core::System::GetInstance().GetJitInterface();
  StaticRecompModuleSource recomp_source;
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    recomp_source =
        StaticRecompModuleSource::Dynamic(impl->config.module.path.string());
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    recomp_source = StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor));
  if (!impl->mods->Empty()) {
    recomp_source.host_call = &ModManager::HostCall;
    recomp_source.host_call_contains = &ModManager::HostCallContains;
    recomp_source.host_call_range_contains =
        &ModManager::HostCallRangeContains;
    recomp_source.host_call_user = impl->mods.get();
  }
  jit.SetStaticRecompModuleSource(std::move(recomp_source));

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  pause_menu::Shutdown();
  RequestStop();
  State::SetOnAfterLoadCallback(nullptr);
  m_impl->present_hook = {};
  m_impl->frame_hook = {};
  m_impl->navigation_hook = {};
  if (s_navigation_overlay_state.load(std::memory_order_acquire) ==
      &m_impl->navigation_overlay)
  {
    VideoCommon::SetExternalOverlayDrawCallback(nullptr);
    s_navigation_overlay_state.store(nullptr, std::memory_order_release);
  }
  if (m_impl->booted) {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->automation_registered) {
    for (int port = 0; port < 4; ++port)
      ciface::Touch::UnregisterGameCubeInputOverrider(port);
  }
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          m_impl->metadata.main_dol.string(), std::move(*s_boot_session_data));
    else if (m_impl->config.load_state_path)
      // DeleteSavestateAfterBoot::No: the state is the player's, not a
      // scratch file this run owns.
      boot = BootParameters::GenerateFromFile(
          m_impl->metadata.main_dol.string(),
          BootSessionData(m_impl->config.load_state_path->string(),
                          DeleteSavestateAfterBoot::No));
    else
      boot =
          BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
    s_boot_session_data.reset();
  }
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  if (m_impl->metadata.disc_id == "GC6E01")
  {
    std::string sixty_fps_status;
    const std::filesystem::path game_ini =
        m_impl->config.user_directory / "GameSettings" / "GC6E01.ini";
    if (!SetColosseum60FpsEnabled(game_ini, m_impl->config.qol.sixty_fps,
                                  &sixty_fps_status))
    {
      m_impl->running = false;
      return {RuntimeExitReason::BootFailed,
              RuntimeError{RuntimeErrorCode::BootFailed,
                           std::move(sixty_fps_status)}};
    }
    std::string idle_skip_status;
    if (!SetColosseumIdleSkipEnabled(game_ini,
                                     m_impl->config.qol.guest_idle_skip,
                                     &idle_skip_status))
    {
      m_impl->running = false;
      return {RuntimeExitReason::BootFailed,
              RuntimeError{RuntimeErrorCode::BootFailed,
                           std::move(idle_skip_status)}};
    }
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        m_impl->navigation_overlay.paused.store(state == Core::State::Paused,
                                                std::memory_order_relaxed);
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  const bool restore_colosseum_guest_hook = m_impl->metadata.disc_id == "GC6E01";
  if (restore_colosseum_guest_hook)
  {
    // Register before BootCore: its CPU thread may load a requested savestate
    // before Runtime::Run regains control.
    State::SetOnAfterLoadCallback(&RestoreColosseumWidescreenGuestHook);
  }
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    if (restore_colosseum_guest_hook)
      State::SetOnAfterLoadCallback(nullptr);
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  m_impl->present_hook =
      GetVideoEvents().after_present_event.Register([this](const PresentInfo& info) {
        m_impl->automation_state.frame_count.store(info.frame_count,
                                                   std::memory_order_relaxed);
        m_impl->automation_state.present_count.store(info.present_count,
                                                     std::memory_order_relaxed);
      });
  m_impl->navigation_overlay.mode.store(
      m_impl->config.graphics.navigation_overlay, std::memory_order_relaxed);
  m_impl->navigation_overlay.overlay_visible.store(
      m_impl->config.graphics.navigation_overlay != 0,
      std::memory_order_relaxed);
  m_impl->navigation_overlay.hires_textures_enabled.store(
      m_impl->config.graphics.hires_textures, std::memory_order_relaxed);
  m_impl->navigation_overlay.community_hd_texture_pack_enabled.store(
      m_impl->config.graphics.community_hd_texture_pack,
      std::memory_order_relaxed);
  m_impl->navigation_overlay.dump_textures_enabled.store(
      m_impl->config.graphics.dump_textures, std::memory_order_relaxed);
  m_impl->navigation_overlay.text_upscale_enabled.store(
      m_impl->config.graphics.text_upscale, std::memory_order_relaxed);
  m_impl->navigation_overlay.input_overlay_enabled.store(
      m_impl->config.graphics.input_overlay, std::memory_order_relaxed);
  m_impl->navigation_overlay.minimap_high_contrast.store(
      m_impl->config.graphics.minimap_high_contrast,
      std::memory_order_relaxed);
  m_impl->navigation_overlay.accessibility_ui_scale.store(
      m_impl->config.graphics.accessibility_ui_scale,
      std::memory_order_relaxed);
  m_impl->navigation_overlay.sixty_fps_enabled.store(
      m_impl->config.qol.sixty_fps, std::memory_order_relaxed);
  m_impl->navigation_overlay.fast_forward_enabled.store(
      m_impl->config.qol.fast_forward, std::memory_order_relaxed);
  m_impl->navigation_overlay.fast_forward_multiplier.store(
      m_impl->config.qol.fast_forward_multiplier, std::memory_order_relaxed);
  m_impl->navigation_overlay.autosave_enabled.store(
      m_impl->config.qol.autosave, std::memory_order_relaxed);
  m_impl->navigation_overlay.autosave_slots.store(
      m_impl->config.qol.autosave_slots, std::memory_order_relaxed);
  m_impl->navigation_overlay.ultrawide_efb_scale.store(
      m_impl->config.graphics.ultrawide_efb_scale,
      std::memory_order_relaxed);
  m_impl->navigation_overlay.ultrawide_gameplay_started.store(
      automation::ShouldStartColosseumUltrawideGameplay(
          m_impl->config.load_state_path.has_value(), false, false),
      std::memory_order_relaxed);
  m_impl->navigation_overlay.standard_efb_scale =
      Config::Get(Config::GFX_EFB_SCALE);
  m_impl->navigation_overlay.savestate_directory =
      m_impl->config.automation.directory / "states";
  m_impl->navigation_overlay.frontend_config_path =
      m_impl->config.user_directory / "config.ini";

  // Savestate save/load and the window menu's Reset live on this state object
  // but are not Colosseum-specific, so publish it for every game. Previously
  // this store sat inside the GC6E01 branch below, which meant Save State
  // silently did nothing and Load State was always empty in any other title:
  // Host_RequestNavigationStateSave() returns early when the pointer is null,
  // so there was no error, no file, and nothing to list.
  //
  // Only the map drawing and the actor-table hook are GC6E01-only. Those read
  // hardcoded addresses (actor table 0x8050EDF0, room id 0x8040837F) that mean
  // nothing in another game, so they stay gated.
  s_navigation_overlay_state.store(&m_impl->navigation_overlay,
                                   std::memory_order_release);

  if (m_impl->metadata.disc_id == "GC6E01")
  {
    if (!m_impl->config.headless)
    {
      const bool widescreen_enabled = m_impl->config.graphics.native_ultrawide;
      std::string widescreen_status;
      if (SetNavigationWidescreenEnabled(widescreen_enabled,
                                         &widescreen_status))
      {
        m_impl->navigation_overlay.widescreen_enabled.store(
            widescreen_enabled, std::memory_order_relaxed);
      }
      m_impl->navigation_overlay.ui_status = std::move(widescreen_status);
      m_impl->frame_hook =
          GetVideoEvents().before_frame_event.Register([this] {
          // Menu constructors can rewrite the widescreen multiplier after
          // vi_end_field, and older savestates can restore it as zero. Enforce
          // a safe projection immediately before the first draw; established
          // gameplay keeps Hor+ even while navigation telemetry is unavailable.
          //
          // Enforce the value the CPU thread decided; do not re-derive it. This
          // callback used to recompute gameplay_ultrawide from the raw,
          // undebounced flags, so it fought the settled decision once per frame
          // and the character model flickered between aspects for the whole
          // transition. Its job is to keep the word from being clobbered, not
          // to have an opinion about what the word should be.
          const u32 projection_bits =
              m_impl->navigation_overlay.guest_projection_bits.load(
                  std::memory_order_acquire);
          auto& memory = Core::System::GetInstance().GetMemory();
          if (memory.Read_U32(0x8047E724) != projection_bits)
            memory.Write_U32(projection_bits, 0x8047E724);
          });
      // The callback also owns PDA side-fill and the optional controller
      // overlay, so keep it registered even when the minimap itself is hidden.
      VideoCommon::SetExternalOverlayDrawCallback(&DrawNavigationOverlay);
    }
    m_impl->navigation_hook =
        GetVideoEvents().vi_end_field_event.Register([this] {
          // Colosseum's 30/60 switch is a single word in the GS graphics
          // state: the frame period the engine waits out between presents.
          // Measured on a static scene, it tracks the rate exactly - 2 on the
          // Phenac field (0.50 XFB promotions per VI field, 29.97 fps) and 1 on
          // the Battle Now menu (1.00 per field, 59.94 fps) - so holding it at
          // 1 makes every scene present every field.
          //
          // It is enforced here rather than patched into the module because the
          // game only writes it on scene transitions: a savestate restores
          // whatever period it was captured with, and a patched setter would
          // never run to correct it. That is why the pre-patched 60 FPS module
          // could not work no matter which instruction it changed.
          //
          // The state lives in the L1 locked cache (its guest pointer reads as
          // 0xE0000000), which the MMU will not resolve, so reach it through
          // the cache directly.
          {
            // The game's own frame period (normally 2 fields = 30 FPS) is saved
            // before the first override and written back when 60 FPS mode is
            // turned off - merely ceasing to force 1 left the game at 60 FPS.
            static u32 s_saved_frame_period = 0;  // big-endian; 0 = not overriding
            if (u8* const l1 = Core::System::GetInstance().GetMemory().GetL1Cache())
            {
              constexpr std::size_t kFramePeriodOffset = 0x58;
              const u32 one = Common::swap32(1u);
              u32 current = 0;
              std::memcpy(&current, l1 + kFramePeriodOffset, sizeof(current));
              if (m_impl->navigation_overlay.sixty_fps_enabled.load(std::memory_order_relaxed))
              {
                if (current != one)
                {
                  if (s_saved_frame_period == 0)
                    s_saved_frame_period = current;
                  std::memcpy(l1 + kFramePeriodOffset, &one, sizeof(one));
                }
                else if (s_saved_frame_period == 0)
                {
                  // Already 1 (e.g. a state saved in 60 FPS mode): the game's
                  // field default is 2.
                  s_saved_frame_period = Common::swap32(2u);
                }
              }
              else if (s_saved_frame_period != 0)
              {
                if (current == one)
                  std::memcpy(l1 + kFramePeriodOffset, &s_saved_frame_period, sizeof(u32));
                s_saved_frame_period = 0;
              }
              const bool loaded =
                  s_state_loaded_frame_period.exchange(false, std::memory_order_acq_rel);
              if (loaded && current == one &&
                  !m_impl->navigation_overlay.sixty_fps_enabled.load(std::memory_order_relaxed))
              {
                // A state saved in 60 FPS mode: back to the game's 2-field period.
                const u32 two = Common::swap32(2u);
                std::memcpy(l1 + kFramePeriodOffset, &two, sizeof(two));
              }
            }
          }
          // GC6E01 field actor transforms. Actor slots are reassigned between
          // rooms, so identify controlled/follower records from their stable
          // archetype and script-state fields.
          constexpr u32 room_id_address = 0x8040837F;
          constexpr u32 menu_active_address = 0x8040836C;
          constexpr u32 field_overlay_mode_address = 0x80402504;
          constexpr u32 field_overlay_phase_address = 0x80402505;
          constexpr u32 dialogue_object_address = 0x80404AD8;
          constexpr u32 active_ui_text_address = 0x80402440;
          constexpr u32 active_ui_message_id_address = 0x80402436;
          // Start of the message window object that owns the id above.
          constexpr u32 message_window_state_address = 0x80402418;
          constexpr u32 movie_ready_address = 0x8047B440;
          constexpr u32 movie_open_address = 0x8047B441;
          constexpr u32 actor_table = 0x8050EDF0;
          // Wes's transform, outside the field actor table.
          //
          // Indoors the actor table holds Rui and every NPC but *not* the
          // player: moving the party and diffing the table showed only Rui's
          // record change. With no player record the scan below finds nothing,
          // navigation_valid goes false, and the minimap, the collision map and
          // every route die with it -- which is why building interiors were
          // completely blind.
          //
          // This address mirrors the player's position and was validated by
          // measurement rather than assumption: it matched the actor table
          // exactly in Phenac City (5.88, 230.38) and the Outskirt Stand
          // (18.00, 35.00) across a process restart and a savestate load, and
          // it tracks movement indoors where the table cannot. Two further
          // addresses (0x80524580, 0x8053D3C4) mirror it identically; this one
          // is used because its Y is the ground plane rather than eye height.
          constexpr u32 player_position_global = 0x80452EE4;
          constexpr u32 actor_stride = 0x170;
          constexpr u32 actor_count = 32;
          constexpr u32 wes_archetype = 0x0B;
          constexpr u32 rui_archetype = 0x2F;
          auto& memory = Core::System::GetInstance().GetMemory();
          // This field reports a complete 4:3 submenu canvas (Pokemon, PDA,
          // and similar screens), not the PDA specifically. Present those
          // canvases at their authored aspect so their edges remain visible
          // on ultrawide displays. Do not use it to select PDA-only artwork.
          const bool menu_active = memory.Read_U32(menu_active_address) == 1;
          // The generic menu bit is also raised for the in-field shop choice
          // and dialogue overlay. Mode 0x18 identifies that field-owned
          // overlay; unlike Pokemon/PDA canvases it must keep the live Hor+
          // world visible behind its orthographic UI.
          const u32 field_overlay_mode = memory.Read_U8(field_overlay_mode_address);
          const u32 field_overlay_phase = memory.Read_U8(field_overlay_phase_address);
          const u32 dialogue_object = memory.Read_U32(dialogue_object_address);
          if (!menu_active)
          {
            m_impl->navigation_overlay.field_overlay_session_active = false;
          }
          else if (field_overlay_mode == 0x18)
          {
            m_impl->navigation_overlay.field_overlay_session_active = true;
          }

          // A savestate may resume directly inside a nested shop inventory
          // panel, before its parent 0x18 field-overlay state is observed.
          // That panel's active UI descriptor starts with the big-endian
          // UTF-16 label "Quantity held". Detect the label instead of tying
          // this presentation rule to a room id or a transient heap address.
          constexpr std::array<u16, 13> quantity_held_label = {
              'Q', 'u', 'a', 'n', 't', 'i', 't', 'y', ' ', 'h', 'e', 'l', 'd'};
          const u32 active_ui_text = memory.Read_U32(active_ui_text_address);
          bool shop_inventory_panel_active = menu_active && active_ui_text >= 0x80000000 &&
                                             active_ui_text <= 0x817FFFFF;
          if (shop_inventory_panel_active)
          {
            for (std::size_t i = 0; i < quantity_held_label.size(); ++i)
            {
              if (memory.Read_U16(active_ui_text + static_cast<u32>(i * 2)) !=
                  quantity_held_label[i])
              {
                shop_inventory_panel_active = false;
                break;
              }
            }
          }
          if (shop_inventory_panel_active)
            m_impl->navigation_overlay.field_overlay_session_active = true;

          // The generic menu bit also covers Pokemon, Items, and shop UI. PDA
          // screens use the stable 0x36xx message family across the main page,
          // Snag List, e-mail, Sort, and Alert Tone panels; use that authored
          // discriminator to extend only the PDA scanline background.
          const u32 active_ui_message_id =
              memory.Read_U16(active_ui_message_id_address);
          // THPPlayerGetState (fn_801E1874) returns active exactly when both
          // of these stable player flags are non-zero. The title handle is
          // populated only while the live-rendered title scene is resident.
          const bool movie_active =
              automation::IsColosseumMoviePlaybackActive(
                  memory.Read_U8(movie_ready_address),
                  memory.Read_U8(movie_open_address));
          // The confirmation dialog alternates its UI message between 0x0069
          // and 0x00ce while the player is still on the naming screen, so the
          // raw predicate blinks and every consumer downstream blinks with it.
          // Hold the naming state across those gaps and release it only after a
          // sustained absence: entering is immediate, leaving takes ~0.4 s.
          // A symmetric N-of-N debounce cannot fix this -- the signal never
          // holds still long enough to satisfy one, which froze the presented
          // state instead of settling it.
          constexpr int kNamingHoldFields = 24;
          const bool character_naming_observed =
              automation::IsColosseumCharacterNamingState(
                  active_ui_message_id, menu_active);
          if (character_naming_observed)
          {
            m_impl->navigation_overlay.naming_hold_fields = kNamingHoldFields;
          }
          else if (m_impl->navigation_overlay.naming_hold_fields > 0)
          {
            --m_impl->navigation_overlay.naming_hold_fields;
          }
          const bool character_naming_active =
              character_naming_observed ||
              m_impl->navigation_overlay.naming_hold_fields > 0;
          // Gate the band on the message window still being resident. The
          // id alone is stale after Quit and would keep the attract title
          // pillarboxed for the rest of the session.
          const bool message_window_live =
              automation::IsColosseumMessageWindowLive(
                  memory.Read_U8(message_window_state_address));
          m_impl->navigation_overlay.front_end_dismiss_hold_fields =
              automation::ResolveColosseumFrontEndDismissHold(
                  message_window_live,
                  m_impl->navigation_overlay.front_end_dismiss_hold_fields);
          const bool front_end_window_resident =
              automation::IsColosseumFrontEndWindowResident(
                  message_window_live,
                  m_impl->navigation_overlay.front_end_dismiss_hold_fields);
          const bool battle_now_observed =
              automation::IsColosseumBattleNowMenu(active_ui_message_id) &&
              front_end_window_resident;
          const bool battle_gameplay_message_observed =
              automation::IsColosseumBattleGameplayMessage(active_ui_message_id);
          if (battle_gameplay_message_observed)
            ++m_impl->navigation_overlay.battle_gameplay_message_fields;
          else
            m_impl->navigation_overlay.battle_gameplay_message_fields = 0;
          // A single stray arena message during a menu transition must not end
          // the authored canvas; a real battle keeps reporting one.
          const bool battle_gameplay_message =
              automation::IsColosseumBattleGameplayConfirmed(
                  m_impl->navigation_overlay.battle_gameplay_message_fields);
          // Colosseum publishes the arena message for a field in the middle of
          // the Battle Now canvas's own transitions, so the id leaves the
          // authored band and comes straight back. Reacting to that single
          // field dropped the authored presentation and the screen flashed
          // Hor+ for the length of the presentation debounce. Hold the
          // classification briefly, and let a confirmed arena clear the hold at
          // once so entering a real battle is not delayed.
          if (battle_now_observed)
          {
            m_impl->navigation_overlay.battle_now_hold_fields =
                automation::kColosseumBattleNowHoldFields;
          }
          else if (m_impl->navigation_overlay.battle_now_hold_fields > 0)
          {
            --m_impl->navigation_overlay.battle_now_hold_fields;
          }
          if (battle_gameplay_message)
            m_impl->navigation_overlay.battle_now_hold_fields = 0;
          const bool battle_now_active =
              battle_now_observed ||
              m_impl->navigation_overlay.battle_now_hold_fields > 0;
          const bool pda_message_active =
              menu_active && automation::IsColosseumPdaMessage(active_ui_message_id);
          if (pda_message_active)
            m_impl->navigation_overlay.field_overlay_session_active = false;

          const bool field_overlay_active =
              menu_active && m_impl->navigation_overlay.field_overlay_session_active;
          const bool complete_menu_active = menu_active && !field_overlay_active;
          const bool pda_active = complete_menu_active && pda_message_active;
          m_impl->navigation_overlay.menu_active.store(
              complete_menu_active, std::memory_order_release);
          m_impl->navigation_overlay.pda_active.store(
              pda_active, std::memory_order_release);
          m_impl->navigation_overlay.field_overlay_active.store(
              field_overlay_active, std::memory_order_release);
          m_impl->navigation_overlay.active_ui_message_id.store(
              active_ui_message_id, std::memory_order_relaxed);
          m_impl->navigation_overlay.published_menu_active.store(
              menu_active, std::memory_order_relaxed);
          m_impl->navigation_overlay.field_overlay_mode.store(
              field_overlay_mode, std::memory_order_relaxed);
          m_impl->navigation_overlay.field_overlay_phase.store(
              field_overlay_phase, std::memory_order_relaxed);
          m_impl->navigation_overlay.dialogue_object.store(
              dialogue_object, std::memory_order_relaxed);
          float player_x = 0.0f;
          float player_z = 0.0f;
          float facing = 0.0f;
          float companion_x = 0.0f;
          float companion_z = 0.0f;
          bool valid = false;
          bool companion_valid = false;
          std::vector<NavigationActor> npcs;
          for (u32 actor_index = 0; actor_index < actor_count; ++actor_index)
          {
            const u32 actor = actor_table + actor_index * actor_stride;
            if (memory.Read_U32(actor + 0x40) != actor + 0x2c ||
                memory.Read_U32(actor + 0x44) != actor + 0x14)
            {
              continue;
            }

            const u32 archetype = memory.Read_U32(actor + 0x74);
            const u32 script_state = memory.Read_U32(actor + 0xD4);
            const u32 script_index = memory.Read_U32(actor + 0xD8);
            const u32 script_pointer = memory.Read_U32(actor + 0xDC);
            const float actor_x = ReadGuestFloat(memory, actor + 8);
            const float actor_z = ReadGuestFloat(memory, actor + 16);
            const bool finite_pose =
                std::isfinite(actor_x) && std::isfinite(actor_z) &&
                std::abs(actor_x) < 100000.0f && std::abs(actor_z) < 100000.0f;
            if (!finite_pose)
              continue;

            if (archetype == wes_archetype && script_state == 0 &&
                script_index == std::numeric_limits<u32>::max() &&
                script_pointer == 0)
            {
              const float actor_facing = ReadGuestFloat(memory, actor + 24);
              if (std::isfinite(actor_facing))
              {
                player_x = actor_x;
                player_z = actor_z;
                facing = actor_facing;
                valid = true;
              }
              continue;
            }

            if (archetype == rui_archetype && script_state == 3 &&
                script_pointer != 0)
            {
              companion_x = actor_x;
              companion_z = actor_z;
              companion_valid = true;
              continue;
            }

            // Anything the room actually placed is worth reporting. Requiring
            // script_state 3 dropped nine of the sixteen valid actors in Phenac
            // City -- including ones standing 29 units from the player, and
            // three that had a live script pointer -- so the minimap and the
            // pathfinder simply could not see them.
            //
            // An unused slot has a recognisable signature rather than a
            // position: never scripted, no script pointer, and parked at the
            // origin. Match that instead of guessing from coordinates alone,
            // because (0, 0) is a legitimate place to stand in some rooms.
            const bool never_scripted =
                script_index == std::numeric_limits<u32>::max() && script_pointer == 0;
            const bool at_origin = actor_x == 0.0f && actor_z == 0.0f;
            if (never_scripted && at_origin)
              continue;
            npcs.push_back({{actor_x, actor_z},
                            actor_index,
                            archetype,
                            script_state,
                            script_state == 3 && script_pointer != 0});
          }
          if (!valid)
          {
            // Only a fallback: the actor table is authoritative when it has the
            // player, because it carries a real facing. This recovers position
            // when it does not.
            const float global_x = ReadGuestFloat(memory, player_position_global);
            const float global_z = ReadGuestFloat(memory, player_position_global + 8);
            if (std::isfinite(global_x) && std::isfinite(global_z) &&
                std::abs(global_x) < 100000.0f && std::abs(global_z) < 100000.0f)
            {
              player_x = global_x;
              player_z = global_z;
              // Facing is not stored beside the position -- turning the player
              // changed no float in that structure, so it is presumably a
              // matrix. Derive a heading from actual movement instead, and hold
              // the last one while standing still, which is what the minimap
              // arrow needs. Radians, matching the actor table's convention.
              const float moved_x = player_x - m_impl->navigation_overlay.last_fallback_x;
              const float moved_z = player_z - m_impl->navigation_overlay.last_fallback_z;
              if (moved_x * moved_x + moved_z * moved_z > 0.25f)
              {
                m_impl->navigation_overlay.last_fallback_facing =
                    std::atan2(moved_x, moved_z);
                m_impl->navigation_overlay.last_fallback_x = player_x;
                m_impl->navigation_overlay.last_fallback_z = player_z;
              }
              facing = m_impl->navigation_overlay.last_fallback_facing;
              valid = true;
            }
          }

          // A story battle keeps the field actor table alive, so navigation stays
          // valid while fighting and the resolver used to rule the battle out. The
          // loaded collision archive settles it instead: Colosseum stages battles
          // on a separate _bf area, so its name is positive evidence of combat
          // where absent navigation data is only evidence of a missing table.
          bool battle_field_archive = false;
          {
            std::lock_guard lock(m_impl->navigation_overlay.mutex);
            battle_field_archive = automation::IsColosseumBattleFieldArchive(
                m_impl->navigation_overlay.collision_archive_name);
            auto& overlay = m_impl->navigation_overlay;
            const unsigned int current_room = memory.Read_U8(room_id_address);
            if (automation::IsColosseumBattleNowArenaArchive(overlay.collision_archive_name) &&
                overlay.collision_room_id == current_room)
              overlay.battle_now_arena_room = current_room;
            if (battle_now_observed || character_naming_active || movie_active ||
                !automation::ShouldRetainColosseumBattleNowArena(
                    overlay.battle_now_arena_room, current_room, valid))
              overlay.battle_now_arena_room = 0;
            // PKMN temporarily tears down navigation/collision data, without
            // changing the actual arena. Preserve its positive identity until
            // the guest changes rooms, rather than treating that gap as a field.
            battle_field_archive = battle_field_archive ||
                automation::ShouldRetainColosseumBattleNowArena(
                    overlay.battle_now_arena_room, current_room, valid);
          }
          m_impl->navigation_overlay.battle_gameplay_session_active =
              automation::ResolveColosseumBattleGameplaySession(
                  m_impl->navigation_overlay.battle_gameplay_session_active,
                  battle_now_active && (!battle_field_archive || battle_now_observed),
                  battle_gameplay_message, valid,
                  character_naming_active, movie_active, battle_field_archive);
          const bool battle_gameplay_active =
              m_impl->navigation_overlay.battle_gameplay_session_active;
          const bool authored_menu_active =
              automation::ShouldUseColosseumAuthoredMenuPresentation(
                  complete_menu_active, valid, character_naming_active,
                  m_impl->navigation_overlay.ultrawide_gameplay_started.load(
                      std::memory_order_relaxed),
                  battle_gameplay_active, battle_now_active);
          // Despite the Config key's name, GFX_WIDESCREEN_AUTHORED_MENU is
          // deliberately naming-only. It drives two things: the model
          // projection scale in VertexShaderManager, which must not touch
          // anything but the naming screens' 3D model, and the 602:480 ->
          // 640:480 presentation override in Present.cpp. Widening it to every
          // authored menu would also widen the presentation override across the
          // whole game; the 602:480 output was checked on screen and is
          // correct, so this stays keyed to naming.
          const bool authored_projection_active =
              character_naming_active;
          const bool live_title_scene_active =
              automation::IsColosseumLiveTitleSceneActive(
                  movie_active, valid,
                  character_naming_active || battle_now_active || battle_gameplay_active);
          m_impl->navigation_overlay.authored_menu_active.store(
              authored_menu_active, std::memory_order_release);
          m_impl->navigation_overlay.authored_projection_active.store(
              authored_projection_active, std::memory_order_release);
          m_impl->navigation_overlay.movie_active.store(
              movie_active, std::memory_order_release);
          m_impl->navigation_overlay.live_title_scene_active.store(
              live_title_scene_active, std::memory_order_release);
          // Computed here, but published from the debounced block below rather
          // than stored immediately: this drives the side-fill the video thread
          // draws, and publishing the raw observation made the margins flash
          // independently of the aspect they are supposed to frame.
          const auto authored_backdrop =
              automation::ResolveColosseumAuthoredBackdrop(
                  m_impl->navigation_overlay.widescreen_enabled.load(
                      std::memory_order_relaxed),
                  movie_active, character_naming_active);
          // Sample the pad for whichever overlay wants to draw it. This used
          // to test only input_overlay_enabled, so the harness controller
          // overlay drew a permanently neutral pad whenever the text overlay
          // happened to be off -- it was reading a snapshot nothing updated.
          bool want_pad_status =
              m_impl->navigation_overlay.input_overlay_enabled.load(
                  std::memory_order_relaxed);
          if (want_pad_status)
          {
            const GCPadStatus pad = Pad::GetStatus(0);
            std::lock_guard lock(m_impl->navigation_overlay.mutex);
            m_impl->navigation_overlay.input_status = pad;
          }
          const bool widescreen_active =
              m_impl->navigation_overlay.widescreen_enabled.load(
                  std::memory_order_relaxed);
          // The field actor table can be unavailable in active rooms and after
          // savestate loads. Any live gameplay menu is enough to establish the
          // session; after that, navigation telemetry must not collapse the
          // camera back to 4:3.
          if (widescreen_active &&
              automation::ShouldStartColosseumUltrawideGameplay(
                  false, valid, menu_active) &&
              !m_impl->navigation_overlay.ultrawide_gameplay_started.load(
                  std::memory_order_relaxed))
          {
            m_impl->navigation_overlay.ultrawide_gameplay_started.store(
                true, std::memory_order_relaxed);
            const NativeUltrawideTarget target = GetNativeUltrawideTarget();
            Config::SetCurrent(Config::GFX_WIDESCREEN_HUD_SAFE_AREA, true);
            Config::SetCurrent(Config::GFX_WIDESCREEN_HUD_SAFE_AREA_SCALE,
                               NativeHudSafeAreaScale(target));
            // The aspect is deliberately not set here. This latch fires on
            // menu_active, i.e. on an authored 4:3 canvas, so forcing Stretch
            // put one wrong frame on screen before the presentation block below
            // corrected it. That block runs in this same callback and now owns
            // the decision; invalidate so it re-applies even if its cache
            // happens to match.
            m_impl->navigation_overlay.presentation_cache_dirty.store(
                true, std::memory_order_release);
          }
          const bool gameplay_started =
              m_impl->navigation_overlay.ultrawide_gameplay_started.load(
                  std::memory_order_relaxed);
          if (automation::ShouldApplyColosseumProjectionPresentation(
                  widescreen_active, gameplay_started, authored_menu_active,
                  live_title_scene_active, movie_active))
          {
            NativeUltrawideTarget target = GetNativeUltrawideTarget();
            const bool gameplay_ultrawide =
                automation::ShouldUseColosseumUltrawideProjection(
                    widescreen_active, gameplay_started,
                    authored_menu_active, live_title_scene_active, movie_active);
            const bool menu_edge_fill =
                authored_backdrop != automation::ColosseumAuthoredBackdrop::None;
            // Menu compositing can temporarily expose an internal 16:9 or
            // 4:3 backbuffer even though the host window is still ultrawide.
            // Keep the last wider physical target so those transient surfaces
            // cannot collapse the Hor+ projection during a menu transition.
            const float cached_aspect =
                m_impl->navigation_overlay.render_surface_aspect.load(
                    std::memory_order_relaxed);
            if (cached_aspect > target.aspect + 0.001f)
            {
              target.width = m_impl->navigation_overlay.render_surface_width.load(
                  std::memory_order_relaxed);
              target.height = m_impl->navigation_overlay.render_surface_height.load(
                  std::memory_order_relaxed);
              target.aspect = cached_aspect;
              target.efb_scale =
                  m_impl->navigation_overlay.render_efb_scale.load(
                      std::memory_order_relaxed);
            }
            // exchange, not load: consume the invalidation so a single outside
            // write forces exactly one re-apply.
            const bool cache_invalidated =
                m_impl->navigation_overlay.presentation_cache_dirty.exchange(
                    false, std::memory_order_acq_rel);
            // Hold the observation for a few fields before acting on it, so a
            // one- or two-field disagreement between the guest words is
            // absorbed instead of being presented. Symmetric: entering and
            // leaving an authored menu are both debounced. ~50 ms at 60 fields.
            constexpr int kPresentationStableFields = 3;
            auto& overlay = m_impl->navigation_overlay;
            if (authored_menu_active != overlay.pending_menu_aspect ||
                authored_projection_active != overlay.pending_menu_projection ||
                character_naming_active != overlay.pending_naming_presentation ||
                gameplay_ultrawide != overlay.pending_field_aspect ||
                movie_active != overlay.pending_movie_aspect ||
                menu_edge_fill != overlay.pending_menu_edge_fill)
            {
              overlay.pending_menu_aspect = authored_menu_active;
              overlay.pending_menu_projection = authored_projection_active;
              overlay.pending_naming_presentation = character_naming_active;
              overlay.pending_field_aspect = gameplay_ultrawide;
              overlay.pending_movie_aspect = movie_active;
              overlay.pending_menu_edge_fill = menu_edge_fill;
              overlay.presentation_stable_fields = 1;
            }
            else if (overlay.presentation_stable_fields < kPresentationStableFields)
            {
              ++overlay.presentation_stable_fields;
            }
            // If the observation never holds still, prefer a late update to a
            // frozen one: after this many fields, act on what we last saw.
            constexpr int kPresentationForceApplyFields = 90;
            ++overlay.presentation_fields_since_apply;
            const bool presentation_settled =
                overlay.presentation_stable_fields >= kPresentationStableFields ||
                overlay.presentation_fields_since_apply >=
                    kPresentationForceApplyFields;
            if (cache_invalidated ||
                (presentation_settled &&
                (authored_menu_active !=
                    m_impl->navigation_overlay.menu_aspect_active ||
                authored_projection_active !=
                    m_impl->navigation_overlay.menu_projection_active ||
                character_naming_active !=
                    m_impl->navigation_overlay.naming_presentation_active ||
                gameplay_ultrawide !=
                    m_impl->navigation_overlay.field_aspect_active ||
                movie_active !=
                    m_impl->navigation_overlay.movie_aspect_active ||
                menu_edge_fill !=
                    m_impl->navigation_overlay.menu_edge_fill_active)))
            {
              m_impl->navigation_overlay.presentation_fields_since_apply = 0;
              m_impl->navigation_overlay.menu_aspect_active = authored_menu_active;
              m_impl->navigation_overlay.menu_projection_active =
                  authored_projection_active;
              m_impl->navigation_overlay.naming_presentation_active =
                  character_naming_active;
              m_impl->navigation_overlay.field_aspect_active = gameplay_ultrawide;
              m_impl->navigation_overlay.movie_aspect_active = movie_active;
              m_impl->navigation_overlay.menu_edge_fill_active = menu_edge_fill;
              m_impl->navigation_overlay.authored_backdrop.store(
                  static_cast<int>(authored_backdrop), std::memory_order_release);
              Config::SetCurrent(Config::GFX_WIDESCREEN_HUD_SAFE_AREA,
                                 gameplay_ultrawide);
              Config::SetCurrent(Config::GFX_WIDESCREEN_AUTHORED_MENU,
                                 authored_projection_active);
              Config::SetCurrent(Config::GFX_COLOSSEUM_NAMING_PRESENTATION,
                                 character_naming_active);
              const AspectMode applied_aspect =
                  (movie_active || authored_menu_active) ? AspectMode::ForceStandard :
                  gameplay_ultrawide ? AspectMode::Stretch :
                                       AspectMode::ForceWide;
              Config::SetCurrent(Config::GFX_ASPECT_RATIO, applied_aspect);
              m_impl->navigation_overlay.published_naming_presentation.store(
                  character_naming_active, std::memory_order_release);
              m_impl->navigation_overlay.published_gameplay_ultrawide.store(
                  gameplay_ultrawide, std::memory_order_release);
              m_impl->navigation_overlay.published_aspect_mode.store(
                  static_cast<int>(applied_aspect), std::memory_order_release);
              m_impl->navigation_overlay.presentation_transitions.fetch_add(
                  1, std::memory_order_relaxed);
            }
            // Complete 2D menu canvases need ForceStandard presentation only.
            // Character naming additionally contains a 3D model and therefore
            // keeps the authored projection correction.
            // Follow the debounced presentation, not the raw observation: the
            // mirror is only updated once the state has settled, so the model
            // scale changes exactly when the aspect does.
            const bool settled_gameplay_ultrawide =
                m_impl->navigation_overlay.field_aspect_active;
            const float guest_projection_aspect =
                settled_gameplay_ultrawide ? target.aspect : kAuthoredGameAspect;
            const u32 projection_bits = std::bit_cast<u32>(
                guest_projection_aspect / kAuthoredGameAspect);
            m_impl->navigation_overlay.guest_projection_bits.store(
                projection_bits, std::memory_order_release);
            if (memory.Read_U32(0x8047E724) != projection_bits)
              memory.Write_U32(projection_bits, 0x8047E724);
            const float previous_aspect =
                m_impl->navigation_overlay.render_surface_aspect.load(
                    std::memory_order_relaxed);
            if (std::abs(previous_aspect - target.aspect) > 0.001f)
            {
              Config::SetCurrent(
                  Config::GFX_WIDESCREEN_HUD_SAFE_AREA_SCALE,
                  NativeHudSafeAreaScale(target));
            }
            m_impl->navigation_overlay.render_surface_width.store(
                target.width, std::memory_order_relaxed);
            m_impl->navigation_overlay.render_surface_height.store(
                target.height, std::memory_order_relaxed);
            m_impl->navigation_overlay.render_surface_aspect.store(
                target.aspect, std::memory_order_relaxed);
            m_impl->navigation_overlay.render_efb_scale.store(
                target.efb_scale, std::memory_order_relaxed);
          }
#ifdef _WIN32
          // MODERNGEKKO_SWITCH_EVENT: this session was launched by the pause
          // menu's Graphics API switch. Once it has been rendering for ~45
          // fields, tell the previous session (still showing its paused frame
          // underneath) that it can hide its window and exit.
          {
            static int switch_fields = -1;
            if (switch_fields < 0)
            {
              const char* event_name = std::getenv("MODERNGEKKO_SWITCH_EVENT");
              switch_fields = event_name && *event_name ? 0 : INT_MAX;
            }
            if (switch_fields != INT_MAX && ++switch_fields == 45)
            {
              // The window thread moves the off-screen window into place and
              // then signals the previous session.
              if (s_platform)
                s_platform->RequestGraphicsSwitchReveal();
              switch_fields = INT_MAX;
            }
          }
#endif
          const unsigned int room_id = valid ? memory.Read_U8(room_id_address) : 0;
          UpdateNavigationOverlayState(m_impl->navigation_overlay, valid, player_x, player_z,
                                       facing, companion_valid, companion_x, companion_z,
                                       room_id, npcs);
          bool needs_collision_refresh = false;
          {
            std::lock_guard lock(m_impl->navigation_overlay.mutex);
            needs_collision_refresh =
                valid && m_impl->navigation_overlay.collision_room_id != room_id;
            if (!needs_collision_refresh && valid &&
                m_impl->navigation_overlay.collision_refresh_retries > 0)
            {
              if (m_impl->navigation_overlay.collision_refresh_delay_frames > 0)
              {
                --m_impl->navigation_overlay.collision_refresh_delay_frames;
              }
              else
              {
                needs_collision_refresh = true;
                m_impl->navigation_overlay.collision_refresh_delay_frames = 30;
                --m_impl->navigation_overlay.collision_refresh_retries;
              }
            }
          }
          if (needs_collision_refresh)
          {
            RefreshNavigationCollisionMap(memory, m_impl->navigation_overlay, room_id,
                                          player_x, player_z);
          }
          std::string collision_archive_name;
          {
            std::lock_guard lock(m_impl->navigation_overlay.mutex);
            collision_archive_name =
                m_impl->navigation_overlay.collision_archive_name;
          }
          UpdateColosseumPyriteSkyCoverage(
              memory, m_impl->navigation_overlay,
              automation::ShouldOffsetColosseumPyriteSky(
                  widescreen_active,
                  m_impl->navigation_overlay.ultrawide_gameplay_started.load(
                      std::memory_order_relaxed),
                  authored_menu_active, room_id, collision_archive_name));
        });
  }
  std::jthread automation_thread;
  // Host actions and recovery saves must also work in ordinary launches.
  // File commands/status remain disabled without an explicit automation root.
  automation_thread = std::jthread([this](std::stop_token stop_token) {
      AutomationLoop(*this, *m_impl, stop_token);
    });
  std::jthread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        Host_UpdateTitle({});
        for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  pause_menu::Initialize(m_impl->config, m_impl->mods.get());
  VideoCommon::SetExternalOverlayDrawCallback(&DrawNavigationOverlay);
  m_impl->platform->MainLoop();
  pause_menu::Shutdown();
  VideoCommon::SetExternalOverlayDrawCallback(nullptr);
  automation_thread.request_stop();
  if (automation_thread.joinable())
    automation_thread.join();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  m_impl->present_hook = {};
  m_impl->frame_hook = {};
  m_impl->navigation_hook = {};
  if (s_navigation_overlay_state.load(std::memory_order_acquire) ==
      &m_impl->navigation_overlay)
  {
    VideoCommon::SetExternalOverlayDrawCallback(nullptr);
    s_navigation_overlay_state.store(nullptr, std::memory_order_release);
  }
  m_impl->platform->SaveWindowGeometry();
  if (restore_colosseum_guest_hook)
    State::SetOnAfterLoadCallback(nullptr);
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  if (!m_impl->config.automation.directory.empty())
    WriteAutomationStatus(m_impl->config.automation.directory, *m_impl);
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko
