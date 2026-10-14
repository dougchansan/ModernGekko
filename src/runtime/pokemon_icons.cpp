// Pokemon Colosseum's own Pokemon face icons for the pause menu.
//
// Extracted from the player's disc (files/poke_face.fsys, tools/
// colosseum_icons/extract_icons.py) into <User>/Load/ColosseumIcons/
// icons_atlas.png and icons.json ({"icons": [{"species", "x", "y", "w",
// "h"}]}, internal species ids).

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"

namespace moderngekko::pause_menu {
namespace {

struct Icon {
  int x, y, w, h;  // in the decoded atlas
  ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
};

// Decoded once; each icon is copied into ImGui's font atlas texture (as a
// custom rectangle) the first time it is drawn. The renderer already manages
// that texture; a separate user texture is not drawn by this backend.
std::vector<u8> s_pixels;
u32 s_width = 0, s_height = 0;
std::unordered_map<int, Icon> s_icons;
ImFontAtlas *s_atlas = nullptr;
bool s_tried = false;

void Load() {
  const std::string directory = File::GetUserPath(D_LOAD_IDX) + "ColosseumIcons/";
  std::string json_text, png;
  if (!File::ReadFileToString(directory + "icons.json", json_text) ||
      !File::ReadFileToString(directory + "icons_atlas.png", png))
    return;
  if (!Common::LoadPNG(std::span(reinterpret_cast<const u8 *>(png.data()), png.size()), &s_pixels,
                       &s_width, &s_height) ||
      s_width == 0 || s_height == 0)
    return;
  picojson::value root;
  if (!picojson::parse(root, json_text).empty() || !root.is<picojson::object>())
    return;
  const auto &object = root.get<picojson::object>();
  const auto icons = object.find("icons");
  if (icons == object.end() || !icons->second.is<picojson::array>())
    return;
  for (const auto &entry : icons->second.get<picojson::array>()) {
    if (!entry.is<picojson::object>())
      continue;
    const auto &icon = entry.get<picojson::object>();
    auto field = [&icon](const char *key) {
      const auto it = icon.find(key);
      return it != icon.end() && it->second.is<double>() ? static_cast<int>(it->second.get<double>()) : -1;
    };
    const int species = field("species"), x = field("x"), y = field("y"), w = field("w"),
              h = field("h");
    if (species < 0 || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        static_cast<u32>(x + w) > s_width || static_cast<u32>(y + h) > s_height)
      continue;
    s_icons[species] = {x, y, w, h};
  }
  std::fprintf(stderr, "[menu] Colosseum icons: %zu species, %ux%u atlas\n", s_icons.size(), s_width,
               s_height);
}

}  // namespace

// The face icon for an internal species id, or false when the icons are not
// installed (the menu then draws no icon box).
bool PokemonIcon(int species, ImTextureRef *texture, ImVec2 *uv0, ImVec2 *uv1) {
  ImFontAtlas *atlas = ImGui::GetIO().Fonts;
  if (!s_tried) {
    s_tried = true;
    Load();
  }
  if (s_atlas != atlas) {
    // A new atlas (video backend restart): copy the icons in again.
    s_atlas = atlas;
    for (auto &[id, icon] : s_icons)
      icon.rect = ImFontAtlasRectId_Invalid;
  }
  const auto it = s_icons.find(species);
  if (it == s_icons.end())
    return false;
  Icon &icon = it->second;
  ImFontAtlasRect r;
  if (icon.rect == ImFontAtlasRectId_Invalid) {
    icon.rect = atlas->AddCustomRect(icon.w, icon.h, &r);
    ImTextureData *tex = atlas->TexData;
    if (icon.rect == ImFontAtlasRectId_Invalid || !tex || tex->Format != ImTextureFormat_RGBA32)
      return false;
    for (int row = 0; row < icon.h; ++row) {
      const u8 *source = s_pixels.data() + (static_cast<std::size_t>(icon.y + row) * s_width + icon.x) * 4;
      std::memcpy(tex->GetPixelsAt(r.x, r.y + row), source, static_cast<std::size_t>(icon.w) * 4);
    }
    atlas->TexPixelsUseColors = true;
    ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, r.w, r.h);
  }
  if (!atlas->GetCustomRect(icon.rect, &r))
    return false;
  *texture = atlas->TexRef;
  *uv0 = r.uv0;
  *uv1 = r.uv1;
  return true;
}

}  // namespace moderngekko::pause_menu
