#include "colosseum_font_extract.hpp"

// Font resource format (from GSmsgFontOpen 0x800FC39C, _msgGetCodeInfo
// 0x800FDF1C and the glyph blitter fn_800FD69C; all big-endian):
//
//   chunk: +0 u16 font_id  +2 u8 cell_w  +3 u8 cell_h  +4 u32 next (0 = last)
//   node (= chunk + 8): +0 u16 count  +2 u16 flag  +4 u32 data_base (rel. node)
//          +0x10 entries[count], sorted by code:
//               u16 code, u8 width (= advance), u8 height (= cell_h),
//               u32 (s8 y_offset << 24) | bitmap_offset (rel. node + data_base)
//   bitmap: linear 4bpp intensity, high nibble = left pixel, ceil(w/2) bytes
//           per row, `height` rows, ink top-aligned.
//   advance = glyph width; code 0x20 advances cell_w / 2.

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

struct Glyph
{
  std::uint16_t code = 0;
  int w = 0, h = 0, y_offset = 0;
  std::vector<std::uint8_t> px;  // w * h, 0..15
  int ink_rows = 0;
  int nonblank_rows = 0;
};

struct Font
{
  int id = 0, cell_w = 0, cell_h = 0;
  std::map<std::uint16_t, Glyph> glyphs;  // first chunk registered wins
};

std::uint32_t U32(const std::vector<std::uint8_t>& b, std::size_t p)
{
  return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) |
         (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
std::uint16_t U16(const std::vector<std::uint8_t>& b, std::size_t p)
{
  return static_cast<std::uint16_t>((b[p] << 8) | b[p + 1]);
}

std::map<int, Font> LoadFonts(const std::vector<std::uint8_t>& blob)
{
  std::map<int, Font> fonts;
  if (blob.size() < 0x20)
    return fonts;
  for (std::size_t p = 0; p + 0x20 < blob.size(); p += 4)
  {
    const int id = U16(blob, p), cw = blob[p + 2], ch = blob[p + 3];
    if (id > 15 || cw != ch || cw < 6 || cw > 48)
      continue;
    const std::size_t node = p + 8;
    const std::uint32_t count = U16(blob, node);
    const std::uint32_t base = U32(blob, node + 4);
    if (count == 0 || count >= 4000 || base != 0x10 + 8 * count)
      continue;
    if (node + 0x10 + 8ull * count > blob.size())
      continue;
    Font& font = fonts[id];
    if (font.glyphs.empty())
    {
      font.id = id;
      font.cell_w = cw;
      font.cell_h = ch;
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
      const std::size_t e = node + 0x10 + 8ull * i;
      Glyph g;
      g.code = U16(blob, e);
      g.w = blob[e + 2];
      g.h = blob[e + 3];
      const std::uint32_t packed = U32(blob, e + 4);
      g.y_offset = static_cast<std::int8_t>(packed >> 24);
      const std::size_t off = node + base + (packed & 0xFFFFFF);
      const std::size_t row_bytes = (static_cast<std::size_t>(g.w) + 1) / 2;
      if (off + row_bytes * g.h > blob.size())
        continue;
      g.px.resize(static_cast<std::size_t>(g.w) * g.h);
      for (int y = 0; y < g.h; ++y)
      {
        bool any = false;
        for (int x = 0; x < g.w; ++x)
        {
          const std::uint8_t byte = blob[off + y * row_bytes + x / 2];
          const std::uint8_t v = (x % 2 == 0) ? (byte >> 4) : (byte & 15);
          g.px[static_cast<std::size_t>(y) * g.w + x] = v;
          any |= v != 0;
        }
        if (any)
        {
          g.ink_rows = y + 1;
          ++g.nonblank_rows;
        }
      }
      font.glyphs.emplace(g.code, std::move(g));
    }
  }
  return fonts;
}

int LineGap(int id, int cell_h)
{
  if (id == 0)
    return 11;
  if (id == 1)
    return 6;
  return static_cast<int>(cell_h * 0.5 + 1.0);
}

std::string FontName(int id)
{
  switch (id)
  {
  case 0: return "colo_font0_22px";
  case 1: return "colo_font1_18px";
  case 2: return "colo_font2_13px";
  case 3: return "colo_font3_18px_condensed";
  case 4: return "colo_font4_13px_condensed";
  default: return "colo_font" + std::to_string(id);
  }
}

void AppendCharJson(std::ostringstream& out, std::uint16_t code)
{
  char buffer[16];
  if (code == '"' || code == '\\')
    out << '\\' << static_cast<char>(code);
  else if (code >= 0x20 && code < 0x7F)
    out << static_cast<char>(code);
  else
  {
    std::snprintf(buffer, sizeof(buffer), "\\u%04x", code);
    out << buffer;
  }
}

bool WriteFont(const Font& font, const fs::path& out_dir, std::string* error)
{
  const std::string name = FontName(font.id);
  int top = 0, bottom = font.cell_h, cell_w = 0;
  for (const auto& [code, g] : font.glyphs)
  {
    top = std::min(top, g.y_offset);
    bottom = std::max(bottom, g.y_offset + g.ink_rows);
    cell_w = std::max(cell_w, g.w);
  }
  const int cell_h = bottom - top;
  const int cols = 32;
  const int rows = static_cast<int>((font.glyphs.size() + cols - 1) / cols);
  const int pw = cell_w + 1, ph = cell_h + 1;
  const int width = cols * pw, height = rows * ph;
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 0);
  for (std::size_t i = 0; i < rgba.size(); i += 4)
    rgba[i] = rgba[i + 1] = rgba[i + 2] = 255;

  std::ostringstream glyphs;
  int index = 0;
  for (const auto& [code, g] : font.glyphs)
  {
    const int cx = (index % cols) * pw, cy = (index / cols) * ph;
    const int gy = cy + (g.y_offset - top);
    for (int y = 0; y < g.ink_rows; ++y)
      for (int x = 0; x < g.w; ++x)
        if (const std::uint8_t v = g.px[static_cast<std::size_t>(y) * g.w + x])
          rgba[(static_cast<std::size_t>(gy + y) * width + cx + x) * 4 + 3] = v * 17;
    const int advance = code == 0x20 ? (font.cell_w >> 1) : g.w;
    glyphs << (index ? ",\n" : "\n") << "  {\"codepoint\": " << code << ", \"char\": \"";
    AppendCharJson(glyphs, code);
    glyphs << "\", \"x\": " << cx << ", \"y\": " << gy << ", \"w\": " << g.w
           << ", \"h\": " << g.ink_rows << ", \"advance\": " << advance
           << ", \"x_offset\": 0, \"y_offset\": " << g.y_offset << ", \"cell_x\": " << cx
           << ", \"cell_y\": " << cy << "}";
    ++index;
  }

  // Baseline: the bottom of 'H'.
  const auto h_glyph = font.glyphs.find('H');
  const int gap = LineGap(font.id, font.cell_h);
  std::ostringstream json;
  json << "{\n \"font_id\": " << font.id << ",\n \"name\": \"" << name << "\",\n"
       << " \"source\": \"sys/main.dol (the font lists also in files/common.fsys :: common_rel)\",\n"
       << " \"cell_w\": " << cell_w << ",\n \"cell_h\": " << cell_h << ",\n"
       << " \"font_cell_w\": " << font.cell_w << ",\n \"font_cell_h\": " << font.cell_h << ",\n"
       << " \"cell_top\": " << top << ",\n \"line_height\": " << font.cell_h + gap << ",\n"
       << " \"line_gap\": " << gap << ",\n \"ascent\": ";
  if (h_glyph != font.glyphs.end())
    json << h_glyph->second.y_offset + h_glyph->second.nonblank_rows;
  else
    json << "null";
  json << ",\n \"space_advance\": " << (font.cell_w >> 1) << ",\n"
       << " \"notes\": \"codepoint == game character code (UTF-16 code unit). x,y,w,h = ink rect in"
          " atlas; draw at (pen_x + x_offset, line_top + y_offset). advance = pen advance.\",\n"
       << " \"glyphs\": [" << glyphs.str() << "\n ]\n}\n";

  const fs::path png = out_dir / (name + "_atlas.png");
  if (!Common::SavePNG(png.string(), rgba.data(), Common::ImageByteFormat::RGBA,
                       static_cast<u32>(width), static_cast<u32>(height), width * 4))
  {
    if (error)
      *error = "could not write " + png.string();
    return false;
  }
  std::ofstream meta(out_dir / (name + "_glyphs.json"), std::ios::binary);
  meta << json.str();
  if (!meta)
  {
    if (error)
      *error = "could not write " + (out_dir / (name + "_glyphs.json")).string();
    return false;
  }
  return true;
}
}  // namespace

bool EnsureColosseumFonts(const fs::path& game_root, const fs::path& out_dir, std::string* error)
{
  std::error_code ec;
  bool present = true;
  for (int id = 0; id <= 4; ++id)
    present = present && fs::is_regular_file(out_dir / (FontName(id) + "_atlas.png"), ec) &&
              fs::is_regular_file(out_dir / (FontName(id) + "_glyphs.json"), ec);
  if (present)
    return true;

  std::ifstream file(game_root / "sys" / "main.dol", std::ios::binary);
  if (!file)
  {
    if (error)
      *error = "no sys/main.dol in " + game_root.string();
    return false;
  }
  const std::vector<std::uint8_t> dol((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
  const std::map<int, Font> fonts = LoadFonts(dol);
  if (fonts.size() < 5)
  {
    if (error)
      *error = "Colosseum fonts not found in main.dol";
    return false;
  }
  fs::create_directories(out_dir, ec);
  for (const auto& [id, font] : fonts)
    if (!WriteFont(font, out_dir, error))
      return false;
  return true;
}
}  // namespace moderngekko::frontend
