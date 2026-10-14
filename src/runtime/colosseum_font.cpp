// Pokemon Colosseum's own text font for the pause menu.
//
// The font is extracted from the player's disc (tools/colosseum_font/
// extract_font.py) into <User>/Load/ColosseumFont/<name>_atlas.png (white
// glyphs, alpha = coverage) and <name>_glyphs.json (cell metrics and per
// glyph rect / advance). It is a bitmap font, so it plugs into ImGui through a
// small ImFontLoader that resamples each glyph to whatever size ImGui bakes.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"

namespace moderngekko::pause_menu {
namespace {

struct BitmapGlyph {
  int x = 0, y = 0, w = 0, h = 0;
  float advance = 0, x_offset = 0, y_offset = 0;
};

struct BitmapFont {
  int width = 0, height = 0;
  std::vector<unsigned char> alpha;  // atlas coverage, width * height
  float cell_height = 1, line_height = 1, ascent = 1;
  std::unordered_map<ImWchar, BitmapGlyph> glyphs;

  float Coverage(float x, float y) const {
    // Bilinear sample, outside the atlas is empty.
    const float fx = x - 0.5f, fy = y - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0, ty = fy - y0;
    auto at = [this](int px, int py) -> float {
      if (px < 0 || py < 0 || px >= width || py >= height)
        return 0.0f;
      return alpha[static_cast<std::size_t>(py) * width + px];
    };
    const float top = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
    const float bottom = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
    return top * (1 - ty) + bottom * ty;
  }
};

std::unique_ptr<BitmapFont> LoadBitmapFont(const std::string &directory, const std::string &name) {
  std::string json_text;
  if (!File::ReadFileToString(directory + name + "_glyphs.json", json_text))
    return nullptr;
  std::string png;
  if (!File::ReadFileToString(directory + name + "_atlas.png", png))
    return nullptr;
  std::vector<u8> rgba;
  u32 w = 0, h = 0;
  if (!Common::LoadPNG(std::span(reinterpret_cast<const u8 *>(png.data()), png.size()), &rgba, &w,
                       &h) ||
      w == 0 || h == 0)
    return nullptr;

  picojson::value root;
  if (!picojson::parse(root, json_text).empty() || !root.is<picojson::object>())
    return nullptr;
  const auto &object = root.get<picojson::object>();
  auto number = [&object](const char *key, double fallback) {
    const auto it = object.find(key);
    return it != object.end() && it->second.is<double>() ? it->second.get<double>() : fallback;
  };

  auto font = std::make_unique<BitmapFont>();
  font->width = static_cast<int>(w);
  font->height = static_cast<int>(h);
  font->alpha.resize(static_cast<std::size_t>(w) * h);
  for (std::size_t i = 0; i < font->alpha.size(); ++i)
    font->alpha[i] = rgba[i * 4 + 3];
  // The game's cell (the em size); the atlas cell may be taller for descenders.
  font->cell_height = static_cast<float>(number("font_cell_h", number("cell_h", 24)));
  font->ascent = static_cast<float>(number("ascent", font->cell_height * 0.8));

  const auto glyphs = object.find("glyphs");
  if (glyphs == object.end() || !glyphs->second.is<picojson::array>())
    return nullptr;
  for (const auto &entry : glyphs->second.get<picojson::array>()) {
    if (!entry.is<picojson::object>())
      continue;
    const auto &g = entry.get<picojson::object>();
    auto field = [&g](const char *key, double fallback) {
      const auto it = g.find(key);
      return it != g.end() && it->second.is<double>() ? it->second.get<double>() : fallback;
    };
    const double codepoint = field("codepoint", -1);
    if (codepoint < 0 || codepoint > 0xFFFF)
      continue;
    BitmapGlyph glyph;
    glyph.x = static_cast<int>(field("x", 0));
    glyph.y = static_cast<int>(field("y", 0));
    glyph.w = static_cast<int>(field("w", 0));
    glyph.h = static_cast<int>(field("h", 0));
    glyph.advance = static_cast<float>(field("advance", glyph.w));
    glyph.x_offset = static_cast<float>(field("x_offset", 0));
    glyph.y_offset = static_cast<float>(field("y_offset", 0));
    font->glyphs[static_cast<ImWchar>(codepoint)] = glyph;
  }
  if (font->glyphs.empty())
    return nullptr;
  // The line is the ink's extent: the game's own line height adds its dialogue
  // line gap, which would offset menu text that is centred on its line.
  float bottom = font->ascent;
  for (const auto &[codepoint, glyph] : font->glyphs)
    if (codepoint < 0x2000 && glyph.h > 0)
      bottom = std::max(bottom, glyph.y_offset + glyph.h);
  font->line_height = bottom;
  std::fprintf(stderr, "[menu] Colosseum font %s: %zu glyphs, %dx%d atlas\n", name.c_str(),
               font->glyphs.size(), font->width, font->height);
  return font;
}

// ImFontConfig::FontData carries a pointer to the BitmapFont.
const BitmapFont *SourceFont(const ImFontConfig *src) {
  return static_cast<const BitmapFont *>(src->FontLoaderData);
}

bool SrcInit(ImFontAtlas *, ImFontConfig *src) {
  const BitmapFont *font = nullptr;
  std::memcpy(&font, src->FontData, sizeof(font));
  src->FontLoaderData = const_cast<BitmapFont *>(font);
  return font != nullptr;
}

void SrcDestroy(ImFontAtlas *, ImFontConfig *src) { src->FontLoaderData = nullptr; }

bool SrcContainsGlyph(ImFontAtlas *, ImFontConfig *src, ImWchar codepoint) {
  return SourceFont(src)->glyphs.contains(codepoint);
}

bool BakedInit(ImFontAtlas *, ImFontConfig *src, ImFontBaked *baked, void *) {
  const BitmapFont *font = SourceFont(src);
  const float scale = baked->Size / font->cell_height;
  baked->Ascent = std::ceil(font->ascent * scale);
  baked->Descent = -std::floor((font->line_height - font->ascent) * scale);
  return true;
}

bool BakedLoadGlyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *,
                    ImWchar codepoint, ImFontGlyph *out_glyph, float *out_advance_x) {
  const BitmapFont *font = SourceFont(src);
  const auto it = font->glyphs.find(codepoint);
  if (it == font->glyphs.end())
    return false;
  const BitmapGlyph &g = it->second;
  const float scale = baked->Size / font->cell_height;
  if (out_advance_x) {
    *out_advance_x = g.advance * scale;
    return true;
  }
  out_glyph->Codepoint = codepoint;
  out_glyph->AdvanceX = g.advance * scale;
  if (g.w <= 0 || g.h <= 0)
    return true;

  const float density = src->RasterizerDensity * baked->RasterizerDensity;
  const float raster = scale * density;
  const int w = std::max(1, static_cast<int>(std::ceil(g.w * raster)));
  const int h = std::max(1, static_cast<int>(std::ceil(g.h * raster)));
  const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, w, h);
  if (pack_id == ImFontAtlasRectId_Invalid)
    return false;
  ImTextureRect *r = ImFontAtlasPackGetRect(atlas, pack_id);

  // Resample: average the source pixels each target pixel covers (shrinking)
  // or bilinear (growing), so the game's bitmap stays smooth at any size.
  std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h);
  const float step = 1.0f / raster;  // source pixels per target pixel
  const int taps = std::clamp(static_cast<int>(std::ceil(step)), 1, 4);
  for (int py = 0; py < h; ++py) {
    for (int px = 0; px < w; ++px) {
      float sum = 0;
      for (int sy = 0; sy < taps; ++sy)
        for (int sx = 0; sx < taps; ++sx)
          sum += font->Coverage(g.x + (px + (sx + 0.5f) / taps) * step,
                                g.y + (py + (sy + 0.5f) / taps) * step);
      pixels[static_cast<std::size_t>(py) * w + px] =
          static_cast<unsigned char>(std::clamp(sum / (taps * taps), 0.0f, 255.0f));
    }
  }

  out_glyph->X0 = g.x_offset * scale;
  out_glyph->Y0 = g.y_offset * scale;
  out_glyph->X1 = out_glyph->X0 + w / density;
  out_glyph->Y1 = out_glyph->Y0 + h / density;
  out_glyph->Visible = true;
  out_glyph->PackId = pack_id;
  ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, pixels.data(),
                                     ImTextureFormat_Alpha8, w);
  return true;
}

const ImFontLoader *Loader() {
  static ImFontLoader loader = [] {
    ImFontLoader l;
    l.Name = "colosseum_bitmap";
    l.FontSrcInit = SrcInit;
    l.FontSrcDestroy = SrcDestroy;
    l.FontSrcContainsGlyph = SrcContainsGlyph;
    l.FontBakedInit = BakedInit;
    l.FontBakedLoadGlyph = BakedLoadGlyph;
    return l;
  }();
  return &loader;
}

std::unique_ptr<BitmapFont> s_font;
ImFont *s_imgui_font = nullptr;
ImFontAtlas *s_atlas = nullptr;
bool s_tried = false;

}  // namespace

// The pause menu's font: Colosseum's when its files are present, else ImGui's.
// Added to the atlas on first use (the renderer manages textures, so the atlas
// accepts new fonts between and during frames).
ImFont *MenuFont() {
  ImFontAtlas *atlas = ImGui::GetIO().Fonts;
  if (s_atlas != atlas) {
    // A new ImGui context (video backend restart) has a new atlas.
    s_atlas = atlas;
    s_imgui_font = nullptr;
    s_tried = false;
  }
  if (!s_tried) {
    s_tried = true;
    const std::string directory = File::GetUserPath(D_LOAD_IDX) + "ColosseumFont/";
    if (!s_font) {
      for (const char *name : {"colo_font0_22px", "menu"}) {
        s_font = LoadBitmapFont(directory, name);
        if (s_font)
          break;
      }
    }
    if (s_font) {
      ImFontConfig config;
      const BitmapFont *pointer = s_font.get();
      config.FontData = const_cast<BitmapFont **>(&pointer);
      config.FontDataSize = sizeof(pointer);
      config.FontDataOwnedByAtlas = false;  // AddFont copies the pointer value
      config.SizePixels = s_font->cell_height;
      config.FontLoader = Loader();
      std::snprintf(config.Name, sizeof(config.Name), "Colosseum");
      s_imgui_font = atlas->AddFont(&config);
    }
  }
  return s_imgui_font ? s_imgui_font : ImGui::GetFont();
}

}  // namespace moderngekko::pause_menu
