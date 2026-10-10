#include "shadow_tracker.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace moderngekko::shadow {
namespace {

// Guest memory, big-endian, every access bounds-checked (the game may be
// loading or resetting while this runs).
class Guest {
public:
  explicit Guest(Memory::MemoryManager &memory) : m_memory(memory) {}

  const std::uint8_t *Bytes(std::uint32_t address, std::uint32_t size) const {
    if (address < 0x80000000u || size == 0)
      return nullptr;
    return m_memory.GetPointerForRange(address, size);
  }
  bool U8(std::uint32_t address, std::uint32_t *out) const {
    const auto *p = Bytes(address, 1);
    if (!p)
      return false;
    *out = p[0];
    return true;
  }
  bool U16(std::uint32_t address, std::uint32_t *out) const {
    const auto *p = Bytes(address, 2);
    if (!p)
      return false;
    *out = (std::uint32_t(p[0]) << 8) | p[1];
    return true;
  }
  bool U32(std::uint32_t address, std::uint32_t *out) const {
    const auto *p = Bytes(address, 4);
    if (!p)
      return false;
    *out = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
    return true;
  }

private:
  Memory::MemoryManager &m_memory;
};

constexpr std::uint32_t kSavePointer = 0x8047ADB8;      // active save
constexpr std::uint32_t kShadowTable = 0x80478F6C;      // definitions, stride 0x38
constexpr std::uint32_t kShadowStride = 0x38;
constexpr int kShadowIds = 97;                          // 0..96, 0 unused
constexpr std::uint32_t kFlagDefinitions = 0x80478F9C;  // 8 bytes per flag id
constexpr std::uint32_t kFlagBanks = 0x80478EEC;        // 8 bytes per bank
constexpr std::uint32_t kSpeciesCount = 0x80478F90;     // -> count
constexpr std::uint32_t kSpeciesTable = 0x80478F94;     // stride 0x11C, name key +0x18
constexpr std::uint32_t kMessageRoot = 0x80478B08;
constexpr std::uint32_t kPokemonSize = 0x138;

// One-bit GSflag: definition (bank = top 2 bits, bit offset u16 +4) -> bank
// state (word count, buffer) -> LSB-numbered bit in a big-endian word.
int Flag(const Guest &g, std::uint32_t flag_id) {
  if (flag_id == 0)
    return -1;
  std::uint32_t definitions, banks;
  if (!g.U32(kFlagDefinitions, &definitions) || !g.U32(kFlagBanks, &banks))
    return -1;
  std::uint32_t kind, bit_offset;
  if (!g.U8(definitions + flag_id * 8, &kind) || !g.U16(definitions + flag_id * 8 + 4, &bit_offset))
    return -1;
  const std::uint32_t bank = kind >> 6;
  std::uint32_t words, buffer;
  if (!g.U32(banks + bank * 8, &words) || !g.U32(banks + bank * 8 + 4, &buffer))
    return -1;
  if ((bit_offset >> 5) >= words)
    return -1;
  std::uint32_t word;
  if (!g.U32(buffer + 4 * (bit_offset >> 5), &word))
    return -1;
  return static_cast<int>((word >> (bit_offset & 31)) & 1);
}

void AppendUtf8(std::string &out, std::uint32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// GSmsgGetGSchar in memory: message groups linked from *(root + 8); each has a
// group id (u16 +0), entry count (u16 +4) and sorted (key, offset) pairs at
// +0x10; the string is big-endian 16-bit text at node + offset.
std::string Message(const Guest &g, std::uint32_t key) {
  if (key == 0)
    return {};
  std::uint32_t root, node;
  if (!g.U32(kMessageRoot, &root) || !g.U32(root + 8, &node))
    return {};
  const std::uint32_t group = key >> 20, local = key & 0xFFFFF;
  for (int guard = 0; node && guard < 64; ++guard) {
    std::uint32_t id, count;
    if (!g.U16(node, &id) || !g.U16(node + 4, &count))
      return {};
    if (id == group && count < 0x10000) {
      std::uint32_t lo = 0, hi = count;
      while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        std::uint32_t entry_key, offset;
        if (!g.U32(node + 0x10 + mid * 8, &entry_key) || !g.U32(node + 0x14 + mid * 8, &offset))
          return {};
        if (entry_key == local) {
          std::string text;
          for (std::uint32_t i = 0; i < 32; ++i) {
            std::uint32_t c;
            if (!g.U16(node + offset + i * 2, &c) || c == 0 || c == 0xFFFF)
              break;
            AppendUtf8(text, c);
          }
          return text;
        }
        if (entry_key < local)
          lo = mid + 1;
        else
          hi = mid;
      }
    }
    if (!g.U32(node + 8, &node))
      return {};
  }
  return {};
}

std::string SpeciesName(const Guest &g, std::uint32_t species) {
  static std::unordered_map<std::uint32_t, std::string> cache;
  if (const auto it = cache.find(species); it != cache.end())
    return it->second;
  std::uint32_t count_pointer, count, table, key;
  if (!g.U32(kSpeciesCount, &count_pointer) || !g.U32(count_pointer, &count) || species >= count ||
      !g.U32(kSpeciesTable, &table) || !g.U32(table + species * 0x11C + 0x18, &key))
    return {};
  std::string name = Message(g, key);
  if (!name.empty())
    cache[species] = name;  // names are stable once resolved
  return name;
}

}  // namespace

Summary Read() {
  Summary summary;
  auto &system = Core::System::GetInstance();
  const Guest g(system.GetMemory());

  std::uint32_t save, table;
  if (!g.U32(kSavePointer, &save) || !g.Bytes(save, 0x1B4BA + kShadowIds * 0x18) ||
      !g.U32(kShadowTable, &table) || !g.Bytes(table, kShadowIds * kShadowStride))
    return summary;

  // Shadow Pokemon the player owns (party first, then the three PC boxes),
  // by Shadow ID: heart gauge while still Shadow.
  std::unordered_map<std::uint32_t, std::pair<float, bool>> owned;
  auto scan = [&](std::uint32_t mon, bool party) {
    std::uint32_t species, id, raw;
    if (!g.U16(mon, &species) || species == 0 || !g.U16(mon + 0xD8, &id) || id == 0 ||
        !g.U32(mon + 0xDC, &raw))
      return;
    const std::int32_t heart = static_cast<std::int32_t>(raw);
    std::uint32_t initial = 0;
    g.U16(save + 0x1B4BA + id * 0x18, &initial);
    const float fraction =
        heart >= 0 && initial ? std::clamp(heart / (100.0f * initial), 0.0f, 1.0f) : -1.0f;
    if (!owned.contains(id) || party)
      owned[id] = {fraction, party};
  };
  for (std::uint32_t slot = 0; slot < 6; ++slot)
    scan(save + 0xA0 + slot * kPokemonSize, true);
  for (std::uint32_t box = 0; box < 3; ++box)
    for (std::uint32_t slot = 0; slot < 30; ++slot)
      scan(save + 0xB9C + box * 0x24A4 + slot * kPokemonSize, false);

  for (int id = 1; id < kShadowIds; ++id) {
    const std::uint32_t record = table + id * kShadowStride;
    std::uint32_t species, snag, purified, seen;
    if (!g.U16(record + 2, &species) || species == 0 || !g.U16(record + 0x12, &snag) ||
        !g.U16(record + 0x14, &purified) || !g.U16(record + 0x16, &seen))
      continue;
    Entry entry;
    entry.shadow_id = id;
    entry.species = static_cast<int>(species);
    entry.name = SpeciesName(g, species);
    if (entry.name.empty())
      entry.name = "#" + std::to_string(species);
    if (Flag(g, purified) == 1)
      entry.status = Status::Purified;
    else if (Flag(g, snag) == 1)
      entry.status = Status::Snagged;
    else if (Flag(g, seen) == 1)
      entry.status = Status::Seen;
    if (const auto it = owned.find(static_cast<std::uint32_t>(id)); it != owned.end()) {
      entry.heart = entry.status == Status::Purified ? -1.0f : it->second.first;
      entry.in_party = it->second.second;
    }
    ++summary.total;
    summary.seen += entry.status != Status::Unseen;
    summary.snagged += entry.status == Status::Snagged || entry.status == Status::Purified;
    summary.purified += entry.status == Status::Purified;
    summary.entries.push_back(std::move(entry));
  }
  summary.valid = summary.total > 0;
  return summary;
}

}  // namespace moderngekko::shadow
