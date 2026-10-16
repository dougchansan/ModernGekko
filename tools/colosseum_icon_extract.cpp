#include "colosseum_icon_extract.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <vector>

#include "Common/Image.h"

namespace moderngekko::frontend
{
namespace
{
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

// common_rel offsets, USA release (validated by the Python extractor).
constexpr std::size_t kStats = 0x123250;  // species stat records, 0x11C each
constexpr std::size_t kFaceTable = 0x1890;
constexpr int kCell = kColosseumIconCell;

std::uint32_t U32(const Bytes& b, std::size_t p)
{
  return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) |
         (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
std::uint16_t U16(const Bytes& b, std::size_t p)
{
  return static_cast<std::uint16_t>((b[p] << 8) | b[p + 1]);
}

// Okumura LZSS: N=4096, F=18, threshold 2, zeroed ring, r starting at N-F.
Bytes Lzss(const std::uint8_t* src, std::size_t n, std::size_t out_size)
{
  constexpr int N = 4096, F = 18, TH = 2;
  std::uint8_t ring[N] = {};
  int r = N - F;
  Bytes out;
  out.reserve(out_size);
  std::size_t i = 0;
  unsigned flags = 0;
  while (out.size() < out_size && i < n)
  {
    flags >>= 1;
    if (!(flags & 0x100))
    {
      flags = src[i++] | 0xFF00;
      if (i >= n)
        break;
    }
    if (flags & 1)
    {
      const std::uint8_t c = src[i++];
      out.push_back(c);
      ring[r] = c;
      r = (r + 1) & (N - 1);
    }
    else
    {
      if (i + 1 >= n)
        break;
      const int a = src[i], b = src[i + 1];
      i += 2;
      const int pos = a | ((b & 0xF0) << 4);
      const int len = (b & 0x0F) + TH + 1;
      for (int k = 0; k < len; ++k)
      {
        const std::uint8_t c = ring[(pos + k) & (N - 1)];
        out.push_back(c);
        ring[r] = c;
        r = (r + 1) & (N - 1);
      }
    }
  }
  out.resize(std::min(out.size(), out_size));
  return out;
}

struct FsysEntry
{
  std::uint32_t id = 0, start = 0, usize = 0, csize = 0;
  std::string name;
};

bool ReadFile(const fs::path& path, Bytes* out)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

bool ParseFsys(const Bytes& d, std::vector<FsysEntry>* entries)
{
  if (d.size() < 0x60 || std::string(d.begin(), d.begin() + 4) != "FSYS")
    return false;
  const std::uint32_t count = U32(d, 0x0C), table = U32(d, 0x40);
  for (std::uint32_t k = 0; k < count; ++k)
  {
    if (table + 4 * k + 4 > d.size())
      return false;
    const std::uint32_t e = U32(d, table + 4 * k);
    if (e + 0x28 > d.size())
      return false;
    FsysEntry entry;
    entry.id = U32(d, e);
    entry.start = U32(d, e + 4);
    entry.usize = U32(d, e + 8);
    entry.csize = U32(d, e + 20);
    const std::uint32_t name = U32(d, e + 0x24);
    for (std::size_t p = name; name && p < d.size() && d[p]; ++p)
      entry.name += static_cast<char>(d[p]);
    if (entry.start + entry.csize > d.size())
      return false;
    entries->push_back(std::move(entry));
  }
  return true;
}

Bytes Extract(const Bytes& d, const FsysEntry& e)
{
  const std::uint8_t* blob = d.data() + e.start;
  if (e.csize >= 0x10 && std::string(blob, blob + 4) == "LZSS")
  {
    const Bytes header(blob, blob + 0x10);
    const std::uint32_t usize = U32(header, 4), csize = std::min<std::uint32_t>(U32(header, 8), e.csize);
    return Lzss(blob + 0x10, csize > 0x10 ? csize - 0x10 : 0, usize);
  }
  return Bytes(blob, blob + std::min(e.usize, e.csize));
}

// A GStexture: tiled GX_RGBA8 (0x45) or GX_RGB5A3 (0x90) portrait.
struct Texture
{
  int w = 0, h = 0;
  std::vector<std::uint8_t> rgba;
};

bool Decode(const Bytes& blob, Texture* tex)
{
  if (blob.size() < 0x80)
    return false;
  const int w = U16(blob, 0), h = U16(blob, 2);
  const std::uint32_t fmt = U32(blob, 8), start = U32(blob, 0x28);
  if ((w != 42 && w != 84) || h != 84 || (fmt != 0x45 && fmt != 0x90))
    return false;
  const int tile_bytes = fmt == 0x45 ? 64 : 32;
  const std::size_t end =
      start + static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * tile_bytes;
  if (start < 0x80 || end > blob.size())
    return false;
  tex->w = w;
  tex->h = h;
  tex->rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
  std::size_t p = start;
  for (int ty = 0; ty < h; ty += 4)
    for (int tx = 0; tx < w; tx += 4)
    {
      for (int i = 0; i < 16; ++i)
      {
        const int x = tx + i % 4, y = ty + i / 4;
        std::uint8_t r, g, b, a;
        if (fmt == 0x45)
        {
          a = blob[p + 2 * i];
          r = blob[p + 2 * i + 1];
          g = blob[p + 32 + 2 * i];
          b = blob[p + 33 + 2 * i];
        }
        else
        {
          const unsigned v = U16(blob, p + 2 * i);
          if (v & 0x8000)
          {
            a = 255;
            auto five = [](unsigned c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
            r = five((v >> 10) & 31);
            g = five((v >> 5) & 31);
            b = five(v & 31);
          }
          else
          {
            const unsigned a3 = (v >> 12) & 7;
            a = static_cast<std::uint8_t>((a3 << 5) | (a3 << 2) | (a3 >> 1));
            r = static_cast<std::uint8_t>(((v >> 8) & 15) * 17);
            g = static_cast<std::uint8_t>(((v >> 4) & 15) * 17);
            b = static_cast<std::uint8_t>((v & 15) * 17);
          }
        }
        if (x < w && y < h)
        {
          std::uint8_t* out = &tex->rgba[(static_cast<std::size_t>(y) * w + x) * 4];
          out[0] = r, out[1] = g, out[2] = b, out[3] = a;
        }
      }
      p += tile_bytes;
    }
  return true;
}

std::vector<int> SpeciesOrder()
{
  std::vector<int> ids;
  for (int s = 1; s <= 251; ++s)
    ids.push_back(s);
  for (int s = 277; s <= 411; ++s)
    ids.push_back(s);
  return ids;
}
}  // namespace

int ColosseumIconIndex(int species)
{
  if (species >= 1 && species <= 251)
    return species - 1;
  if (species >= 277 && species <= 411)
    return 251 + (species - 277);
  return -1;
}

bool EnsureColosseumIcons(const fs::path& game_root, const fs::path& out_dir, std::string* error)
{
  auto fail = [&](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };
  std::error_code ec;
  if (fs::is_regular_file(out_dir / "icons_atlas.png", ec) &&
      fs::is_regular_file(out_dir / "icons.json", ec))
    return true;

  Bytes common, faces;
  if (!ReadFile(game_root / "files" / "common.fsys", &common) ||
      !ReadFile(game_root / "files" / "poke_face.fsys", &faces))
    return fail("no files/common.fsys or files/poke_face.fsys in " + game_root.string());
  std::vector<FsysEntry> common_entries, face_entries;
  if (!ParseFsys(common, &common_entries) || !ParseFsys(faces, &face_entries))
    return fail("unreadable FSYS archive");
  const auto rel_entry = std::ranges::find(common_entries, std::string("common_rel"), &FsysEntry::name);
  if (rel_entry == common_entries.end())
    return fail("common_rel not found");
  const Bytes rel = Extract(common, *rel_entry);
  if (face_entries.size() != 418 || rel.size() < kStats + 412 * 0x11C)
    return fail("not the USA (GC6E01) portrait archive");

  std::map<std::uint32_t, Texture> textures;
  for (const FsysEntry& entry : face_entries)
  {
    const Bytes blob = Extract(faces, entry);
    Texture tex;
    if (blob.size() != entry.usize || !Decode(blob, &tex))
      return fail("unexpected portrait resource " + entry.name);
    textures[entry.id] = std::move(tex);
  }

  const std::vector<int> species_ids = SpeciesOrder();
  const int width = kColosseumIconColumns * kCell, height = 25 * kCell;
  std::vector<std::uint8_t> atlas(static_cast<std::size_t>(width) * height * 4, 0);
  std::ostringstream icons;
  for (std::size_t i = 0; i < species_ids.size(); ++i)
  {
    const int species = species_ids[i];
    unsigned key = U16(rel, kStats + species * 0x11C + 0x10E);
    if (species == 201)
      key = 0x305;  // Unown A
    const std::size_t p = kFaceTable + key * 8;
    if (p + 8 > rel.size())
      return fail("face table out of range");
    const auto it = textures.find(U32(rel, p + 4));
    if (it == textures.end())
      return fail("unmapped species " + std::to_string(species));
    const int crop_y = (rel[p] & 0x80) ? kCell : 0;
    const int x = static_cast<int>(i % kColosseumIconColumns) * kCell,
              y = static_cast<int>(i / kColosseumIconColumns) * kCell;
    for (int row = 0; row < kCell; ++row)
      std::copy_n(&it->second.rgba[(static_cast<std::size_t>(crop_y + row) * it->second.w) * 4],
                  kCell * 4, &atlas[(static_cast<std::size_t>(y + row) * width + x) * 4]);
    icons << (i ? ",\n" : "\n") << "    {\n      \"species\": " << species
          << ",\n      \"x\": " << x << ",\n      \"y\": " << y << ",\n      \"w\": " << kCell
          << ",\n      \"h\": " << kCell << "\n    }";
  }

  fs::create_directories(out_dir, ec);
  const fs::path png = out_dir / "icons_atlas.png";
  if (!Common::SavePNG(png.string(), atlas.data(), Common::ImageByteFormat::RGBA,
                       static_cast<u32>(width), static_cast<u32>(height), width * 4))
    return fail("could not write " + png.string());
  std::ofstream json(out_dir / "icons.json", std::ios::binary);
  json << "{\n  \"cell_w\": " << kCell << ",\n  \"cell_h\": " << kCell << ",\n  \"icons\": ["
       << icons.str() << "\n  ]\n}\n";
  if (!json)
    return fail("could not write icons.json");
  return true;
}
}  // namespace moderngekko::frontend
