#include "automation_protocol.hpp"
#include "cache_affinity.hpp"
#include "colosseum_font_extract.hpp"
#include "colosseum_icon_extract.hpp"
#include "controller_bindings.hpp"
#include "frontend_config.hpp"
#include "launcher_savestates.hpp"
#include "dol_patch.hpp"
#include "launcher_theme.hpp"
#include "memory_card_library.hpp"
#include "module_setup.hpp"
#include "onscreen_keyboard.hpp"
#include "moderngekko/game.hpp"
#include "moderngekko/hd_texture_pack.hpp"
#include "netplay_session.hpp"

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Image.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

#include <SDL3/SDL.h>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif
#include <imgui.h>
#include <imgui_internal.h>  // ImGui::ClearActiveID
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace theme = moderngekko::frontend::theme;

namespace
{
#ifndef MODERNGEKKO_FRONTEND_NAME
#define MODERNGEKKO_FRONTEND_NAME "ModernGekko"
#endif

#ifndef MODERNGEKKO_RUNNER_FILENAME
#define MODERNGEKKO_RUNNER_FILENAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

#ifndef MODERNGEKKO_LOG_FILENAME
#define MODERNGEKKO_LOG_FILENAME "ModernGekko.log"
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr std::string_view CONTROLLER_PROFILE_NAME = "GCPadNew.ini";
constexpr std::string_view GENERATED_PROFILE_LABEL = "GameCube profile";
#else
constexpr std::string_view CONTROLLER_PROFILE_NAME = "WiimoteNew.ini";
constexpr std::string_view GENERATED_PROFILE_LABEL = "sideways profile";
#endif

struct ExtractionState
{
  std::atomic<bool> running{false};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> total{1};
  std::mutex mutex;
  std::string status;
  std::string error;
  std::optional<fs::path> finished_game;
  std::atomic<bool> module_ready{false};  // a module build (without extraction) finished
};

struct DialogState
{
  std::mutex mutex;
  std::optional<fs::path> selected;
  std::string error;
};

struct ControllerOption
{
  std::string label;
  std::string device;
};

std::string RequiredGameError(const moderngekko::GameMetadata& metadata)
{
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (metadata.disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return "expected disc ID " MODERNGEKKO_REQUIRED_DISC_ID ", found " + metadata.disc_id;
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (metadata.dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256)
    return "main DOL does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (metadata.rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256)
    return "_Main.rel does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (metadata.assets_sha256 != MODERNGEKKO_REQUIRED_ASSETS_SHA256)
    return "game assets do not match the pinned release";
#endif
  return {};
}

std::vector<ControllerOption> EnumerateControllers()
{
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  SDL_JoystickID* joystick_ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i)
  {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    const bool gamepad = SDL_IsGamepad(joystick_id);
    const char* name_value =
        gamepad ? SDL_GetGamepadNameForID(joystick_id) : SDL_GetJoystickNameForID(joystick_id);
    const std::string name = name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    if (!gamepad)
      continue;
    ControllerOption option;
    option.label = device_id == 0 ? name : name + " (" + std::to_string(device_id + 1) + ")";
    option.device = "SDL/" + std::to_string(device_id) + "/" + name;
    result.emplace_back(std::move(option));
  }
  SDL_free(joystick_ids);
  // The keyboard is always there, after the gamepads, so a player without a
  // pad can pick it and the first run falls back to it.
  result.push_back({"Keyboard", std::string(moderngekko::frontend::KeyboardDevice())});
  return result;
}

int FindController(const std::vector<ControllerOption>& controllers, std::string_view device)
{
  const auto found = std::ranges::find(controllers, device, &ControllerOption::device);
  return found == controllers.end() ? -1 : static_cast<int>(found - controllers.begin());
}

fs::path DefaultUserDirectory(const fs::path& release_directory)
{
#ifdef MODERNGEKKO_PORTABLE_USER_DIRECTORY
  return release_directory / "User";
#else
#ifdef MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
#if defined(_WIN32)
  if (const char* user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA"))
    return fs::path(local_app_data) / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char* xdg = std::getenv("XDG_DATA_HOME"))
    return fs::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".local/share" / MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
#endif
}

fs::path DocumentsDirectory()
{
#if defined(_WIN32)
  if (const char* user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents";
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Documents";
  return fs::current_path();
}

fs::path ExecutableDirectory(const char* argv0)
{
  std::error_code ec;
#if defined(__linux__)
  const fs::path proc_executable = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const fs::path executable = fs::weakly_canonical(argv0, ec);
  return ec ? fs::current_path() : executable.parent_path();
}

fs::path SiblingExecutable(const fs::path& release_directory, std::string name)
{
#if defined(_WIN32)
  name += ".exe";
#endif
  const fs::path sibling = release_directory / name;
  return fs::is_regular_file(sibling) ? sibling : fs::path(name);
}

bool ApplyBundledDolPatch(const fs::path& game_root, const fs::path& release_directory,
                          bool* changed, std::string* error)
{
#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
  if (game_root.empty() || !fs::is_directory(game_root))
  {
    *changed = false;
    return true;
  }
  return moderngekko::frontend::ApplyDolPatchManifest(
      game_root / "sys" / "main.dol", release_directory / MODERNGEKKO_DOL_PATCH_MANIFEST,
      changed, error);
#else
  *changed = false;
  return true;
#endif
}

fs::path DefaultGameFile(const fs::path& user_directory, const fs::path& release_directory)
{
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  return release_directory / "default-game.txt";
#else
  return user_directory / "default-game.txt";
#endif
}

fs::path ReadDefaultGame(const fs::path& user_directory, const fs::path& release_directory)
{
  std::ifstream file(DefaultGameFile(user_directory, release_directory));
  std::string value;
  std::getline(file, value);
  if (!value.empty() && value.back() == '\r')
    value.pop_back();
  fs::path game(value);
  if (game.is_relative())
    game = release_directory / game;
  return game;
}

bool WriteDefaultGame(const fs::path& user_directory, const fs::path& release_directory,
                      const fs::path& game, std::string* error)
{
  const fs::path destination = DefaultGameFile(user_directory, release_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  std::ofstream file(destination, std::ios::trunc);
  if (!file)
  {
    if (error)
      *error = "can't save default-game.txt";
    return false;
  }
  fs::path stored = game;
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path relative = fs::relative(game, release_directory, ec);
  if (!ec && !relative.empty())
    stored = relative;
#endif
  file << stored.string() << '\n';
  return true;
}

std::vector<fs::path> FindDiscImages()
{
  std::vector<fs::path> images;
  std::error_code ec;
  const fs::path documents = DocumentsDirectory();
  if (!fs::is_directory(documents, ec))
    return images;
  fs::recursive_directory_iterator iterator(documents,
                                            fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (iterator != end)
  {
    if (iterator.depth() > 4)
      iterator.disable_recursion_pending();
    if (iterator->is_regular_file(ec))
    {
      std::string extension = iterator->path().extension().string();
      std::ranges::transform(extension, extension.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".wbfs" || extension == ".iso" || extension == ".rvz")
      {
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
        const std::unique_ptr<DiscIO::Volume> volume =
            DiscIO::CreateVolume(iterator->path().string());
        if (volume)
        {
          const std::string disc_id = volume->GetGameID(volume->GetGamePartition());
          bool supported = disc_id == MODERNGEKKO_REQUIRED_DISC_ID;
#ifdef MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID
          supported = supported || disc_id == MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID;
#endif
          if (supported)
            images.push_back(iterator->path());
        }
#else
        images.push_back(iterator->path());
#endif
      }
    }
    iterator.increment(ec);
    if (ec)
      ec.clear();
  }
  std::ranges::sort(images);
  return images;
}

std::optional<fs::path> PrepareDisc(const fs::path& image, const fs::path& user_directory,
                                    const fs::path& release_directory, ExtractionState* state,
                                    std::string* error)
{
  std::unique_ptr<DiscIO::Volume> source = DiscIO::CreateVolume(image.string());
  if (!source)
  {
    *error = "Dolphin rejected the selected disc image";
    return std::nullopt;
  }
  const std::string source_id = source->GetGameID(source->GetGamePartition());
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (source_id == MODERNGEKKO_REQUIRED_DISC_ID)
    return image;
#endif
#if defined(MODERNGEKKO_DISC_PREPARER_FILENAME) && \
    defined(MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID) && defined(MODERNGEKKO_REQUIRED_DISC_ID)
  if (source_id != MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID)
  {
    *error = "expected clean " MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID " or patched "
             MODERNGEKKO_REQUIRED_DISC_ID "; selected " + source_id;
    return std::nullopt;
  }
  const fs::path output = user_directory / "Setup" / "SonicRidersTE-2.4.6.1.iso";
  std::error_code ec;
  fs::create_directories(output.parent_path(), ec);
  if (ec)
  {
    *error = "can't create setup directory: " + ec.message();
    return std::nullopt;
  }
  {
    std::lock_guard lock(state->mutex);
    state->status = "Applying Tournament Edition 2.4.6.1";
  }
  const fs::path preparer =
      SiblingExecutable(release_directory, MODERNGEKKO_DISC_PREPARER_FILENAME);
  const std::array<std::string, 5> storage = {
      preparer.string(), "--input", image.string(), "--output", output.string()};
  std::array<const char*, 6> arguments{};
  for (std::size_t i = 0; i < storage.size(); ++i)
    arguments[i] = storage[i].c_str();
  SDL_Process* process = SDL_CreateProcess(arguments.data(), false);
  if (!process)
  {
    *error = "can't start Tournament Edition patcher: " + std::string(SDL_GetError());
    return std::nullopt;
  }
  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  if (!waited || exit_code != 0)
  {
    *error = "Tournament Edition patching failed";
    return std::nullopt;
  }
  return output;
#else
  *error = "this disc is not the pinned patched release";
  return std::nullopt;
#endif
}

// Builds the native game module from the extracted game if the runner would not find one
// (release packages carry no game-derived code). Progress goes to `state`.
bool EnsureGameModule(const fs::path& game, const fs::path& user_directory,
                      const fs::path& release_directory, ExtractionState* state,
                      std::string* error)
{
  const auto inspected = moderngekko::InspectGame(game);
  if (!inspected)
    return true;  // the caller reports an invalid game
  const std::string& disc_id = inspected.metadata->disc_id;
  if (moderngekko::frontend::HasGameModule(release_directory, user_directory, disc_id))
    return true;
  if (!moderngekko::frontend::CanBuildGameModule(release_directory))
  {
    *error = "The game module is missing and this release cannot build it.";
    return false;
  }
  return moderngekko::frontend::BuildGameModule(
      game, disc_id, release_directory, user_directory,
      [state](const std::string& status, unsigned done, unsigned total)
      {
        state->total = std::max(1u, total);
        state->completed = total ? done : 0;
        std::lock_guard lock(state->mutex);
        state->status = status;
      },
      error);
}

bool ExtractDisc(const fs::path& image, const fs::path& user_directory,
                 const fs::path& release_directory, ExtractionState* state)
{
  auto fail = [&](std::string message)
  {
    std::lock_guard lock(state->mutex);
    state->error = std::move(message);
    state->running = false;
    return false;
  };

  {
    std::lock_guard lock(state->mutex);
    state->status = "Opening " + image.filename().string();
  }
  std::string preparation_error;
  const std::optional<fs::path> prepared =
      PrepareDisc(image, user_directory, release_directory, state, &preparation_error);
  if (!prepared)
    return fail(std::move(preparation_error));
  std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(prepared->string());
  if (!volume)
    return fail("Dolphin rejected the prepared disc image");

  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem* filesystem = volume->GetFileSystem(partition);
  if (!filesystem || !filesystem->IsValid())
    return fail("Dolphin could not read the game partition filesystem");

  std::string disc_id = volume->GetGameID(partition);
  if (disc_id.size() != 6)
    return fail("the selected image has an invalid disc ID");
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return fail("this frontend requires disc ID " MODERNGEKKO_REQUIRED_DISC_ID "; selected " +
                disc_id);
#endif
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path games_directory = release_directory;
  const fs::path output = games_directory / "Game";
#else
  const fs::path games_directory = user_directory / "games";
  const fs::path output = games_directory / disc_id;
#endif
  std::error_code ec;
  if (fs::is_directory(output, ec))
  {
    bool changed = false;
    std::string patch_error;
    if (!ApplyBundledDolPatch(output, release_directory, &changed, &patch_error))
      return fail("existing extraction patching failed: " + patch_error);
  }
  const auto existing = moderngekko::InspectGame(output);
  if (existing && RequiredGameError(*existing.metadata).empty())
  {
    std::string error;
    if (!WriteDefaultGame(user_directory, release_directory, output, &error))
      return fail(error);
    if (!EnsureGameModule(output, user_directory, release_directory, state, &error))
      return fail(error);
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Using existing extraction";
    state->running = false;
    return true;
  }

  const fs::path staging = games_directory / (disc_id + ".extracting");
  fs::remove_all(staging, ec);
  fs::create_directories(staging / "files", ec);
  if (ec)
    return fail("can't create extraction directory: " + ec.message());

  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting system data";
  }
  if (!DiscIO::ExportSystemData(*volume, partition, staging.string()))
  {
    fs::remove_all(staging, ec);
    return fail("Dolphin failed while extracting the disc system data");
  }

  state->total = std::max(1u, filesystem->GetRoot().GetTotalChildren());
  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting game files";
  }
  DiscIO::ExportDirectory(*volume, partition, filesystem->GetRoot(), true, "",
                          (staging / "files").string(),
                          [state](const std::string& path)
                          {
                            ++state->completed;
                            std::lock_guard lock(state->mutex);
                            state->status = "Extracting " + path;
                            return false;
                          });

  bool dol_changed = false;
  std::string dol_patch_error;
  if (!ApplyBundledDolPatch(staging, release_directory, &dol_changed, &dol_patch_error))
  {
    fs::remove_all(staging, ec);
    return fail("extracted DOL patching failed: " + dol_patch_error);
  }

  const auto inspected = moderngekko::InspectGame(staging);
  if (!inspected)
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + inspected.error);
  }
  if (const std::string identity_error = RequiredGameError(*inspected.metadata);
      !identity_error.empty())
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + identity_error);
  }

  fs::remove_all(output, ec);
  ec.clear();
  fs::rename(staging, output, ec);
  if (ec)
    return fail("can't publish extracted game: " + ec.message());
  std::string error;
  if (!WriteDefaultGame(user_directory, release_directory, output, &error))
    return fail(error);
  if (!EnsureGameModule(output, user_directory, release_directory, state, &error))
    return fail(error);

  {
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Extraction complete";
  }
  state->running = false;
  return true;
}

// Game Boy Advance link settings, kept in config.ini where the runtime's
// Players panel reads and writes them: [GBA] rom/bios, and [Players] port2.
struct GbaSettings
{
  fs::path rom;
  fs::path bios;
  bool player2 = false;  // player 2 plays on an emulated GBA
};

GbaSettings ReadGbaSettings(const fs::path& user_directory)
{
  Common::IniFile ini;
  ini.Load((user_directory / "config.ini").string());
  GbaSettings settings;
  std::string value;
  if (ini.GetOrCreateSection("GBA")->Get("rom", &value))
    settings.rom = value;
  if (ini.GetOrCreateSection("GBA")->Get("bios", &value))
    settings.bios = value;
  settings.player2 = ini.GetOrCreateSection("Players")->Get("port2", &value) && value == "gba";
  return settings;
}

void WriteGbaSettings(const fs::path& user_directory, const GbaSettings& settings)
{
  const std::string path = (user_directory / "config.ini").string();
  Common::IniFile ini;
  ini.Load(path);
  auto* gba = ini.GetOrCreateSection("GBA");
  gba->Set("rom", settings.rom.string());
  gba->Set("bios", settings.bios.string());
  ini.GetOrCreateSection("Players")->Set("port2", std::string(settings.player2 ? "gba" : "controller"));
  ini.Save(path);
}

// Colosseum Battle online ([OnlineBattle] in config.ini). The online battle
// mod reads turn_seconds / auto_choose; the address and port reach it as
// --env COLO_ONLINE_* when the launcher starts a battle. The keys avoid the
// names the frontend config writer owns in every section (address, port).
struct OnlineBattleSettings
{
  std::string address = "127.0.0.1";
  int port = 27100;
  int seat = 1;  // the host's seat; the joining player takes the other
  int turn_seconds = 0;
  bool auto_choose = true;
};

OnlineBattleSettings ReadOnlineBattleSettings(const fs::path& user_directory)
{
  Common::IniFile ini;
  ini.Load((user_directory / "config.ini").string());
  auto* section = ini.GetOrCreateSection("OnlineBattle");
  OnlineBattleSettings settings;
  section->Get("battle_address", &settings.address, settings.address);
  section->Get("battle_port", &settings.port, settings.port);
  section->Get("seat", &settings.seat, settings.seat);
  section->Get("turn_seconds", &settings.turn_seconds, settings.turn_seconds);
  section->Get("auto_choose", &settings.auto_choose, settings.auto_choose);
  settings.port = std::clamp(settings.port, 1, 65535);
  settings.seat = settings.seat == 2 ? 2 : 1;
  return settings;
}

void WriteOnlineBattleSettings(const fs::path& user_directory, const OnlineBattleSettings& settings)
{
  const std::string path = (user_directory / "config.ini").string();
  Common::IniFile ini;
  ini.Load(path);
  auto* section = ini.GetOrCreateSection("OnlineBattle");
  section->Set("battle_address", settings.address);
  section->Set("battle_port", settings.port);
  section->Set("seat", settings.seat);
  section->Set("turn_seconds", settings.turn_seconds);
  section->Set("auto_choose", settings.auto_choose);
  ini.Save(path);
}

// The pad that plays player 2's GBA: the first gamepad that is not player
// 1's, or player 1's own when it is the only one. Buttons sit where a
// handheld has them (A on the right face button, B below it).
void MapGbaPad(const fs::path& user_directory, const std::string& device)
{
  const std::string path = (user_directory / "Config" / "GBA.ini").string();
  Common::IniFile ini;
  ini.Load(path);
  auto* map = ini.GetOrCreateSection("GBA2");
  map->Set("Device", device);
  map->Set("Buttons/B", std::string("`Button A`"));
  map->Set("Buttons/A", std::string("`Button B`"));
  map->Set("Buttons/L", std::string("`Shoulder L` | `Trigger L`"));
  map->Set("Buttons/R", std::string("`Shoulder R` | `Trigger R`"));
  map->Set("Buttons/SELECT", std::string("Back"));
  map->Set("Buttons/START", std::string("Start"));
  map->Set("D-Pad/Up", std::string("`Pad N` | `Left Y+`"));
  map->Set("D-Pad/Down", std::string("`Pad S` | `Left Y-`"));
  map->Set("D-Pad/Left", std::string("`Pad W` | `Left X-`"));
  map->Set("D-Pad/Right", std::string("`Pad E` | `Left X+`"));
  ini.Save(path);
}

void SDLCALL FileDialogCallback(void* userdata, const char* const* filelist, int)
{
  auto* state = static_cast<DialogState*>(userdata);
  std::lock_guard lock(state->mutex);
  if (!filelist)
    state->error = SDL_GetError();
  else if (filelist[0])
    state->selected = filelist[0];
}

fs::path SiblingRunner(const char* argv0)
{
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path runner = MODERNGEKKO_RUNNER_FILENAME;
#if defined(_WIN32)
  runner += ".exe";
#endif
  const fs::path sibling = self.parent_path() / runner;
  return fs::is_regular_file(sibling) ? sibling : runner;
}

// ---- launcher pages ----
constexpr const char* kTitle = "POK\xC3\xA9MON COLOSSEUM";

enum Page
{
  kPlay,
  kCards,
  kStates,
  kDisplay,
  kGameplay,
  kControllers,
  kMods,
  kNetplay,
  kOnlineBattle,
  kPageCount,
};
constexpr std::array<const char*, kPageCount> kPageNames = {
    "PLAY",     "MEMORY CARDS", "SAVE STATES",     "DISPLAY",      "GAMEPLAY",
    "CONTROLLERS",  "MODS & OVERLAYS", "NETPLAY", "ONLINE BATTLE"};
constexpr std::array<const char*, kPageCount> kPageHelp = {
    "Start the game, from the memory card or from a save state.",
    "Your memory cards. Each card is one Colosseum save; the one in Slot A is played.",
    "Save states from the pause menu and the room autosaves, newest first.",
    "Window mode, render resolution, aspect ratio and texture filtering.",
    "Frame rate, speed and the safety nets: autosaves and stall recovery.",
    "Which gamepad or keyboard plays, its button mapping, and the on-screen input display.",
    "Minimap, overlays, and the HD texture and text upgrades.",
    "Play with a friend over the internet. Still experimental.",
    "Colosseum Battle against a friend's own save, each on their own game."};

std::string Upper(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

// "today 14:02", "yesterday 09:15", "Oct 13  20:30".
std::string DescribeTime(fs::file_time_type when)
{
  // Through "now" on both clocks: libc++ (macOS) has no std::chrono::clock_cast.
  const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      when - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  const std::time_t then = std::chrono::system_clock::to_time_t(system);
  const std::time_t now = std::time(nullptr);
  const std::tm a = State::Layout::LocalTime(then), b = State::Layout::LocalTime(now);
  char text[32] = {};
  std::tm yesterday = b;
  yesterday.tm_mday -= 1;
  std::mktime(&yesterday);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
    std::strftime(text, sizeof(text), "today %H:%M", &a);
  else if (a.tm_year == yesterday.tm_year && a.tm_yday == yesterday.tm_yday)
    std::strftime(text, sizeof(text), "yesterday %H:%M", &a);
  else
    std::strftime(text, sizeof(text), "%b %d  %H:%M", &a);
  return text;
}

struct StateEntry
{
  std::string title, kind, when, file;
  fs::path path;
};

StateEntry DescribeState(const fs::path& path)
{
  StateEntry entry;
  entry.file = path.filename().string();
  entry.path = path;
  std::string stem = path.stem().string();
  const bool automatic = stem.starts_with(State::Layout::AUTOMATIC_PREFIX);
  entry.kind = automatic ? "AUTOSAVE" : "SAVE STATE";
  // Whatever follows the prefix that is not the timestamp is the game's own
  // name for the moment (a room, a checkpoint) or the player's.
  const std::string_view prefix =
      automatic ? State::Layout::AUTOMATIC_PREFIX : State::Layout::MANUAL_PREFIX;
  if (stem.starts_with(prefix))
    stem.erase(0, prefix.size());
  std::string name;
  for (std::size_t start = 0; start <= stem.size();)
  {
    std::size_t end = stem.find('-', start);
    if (end == std::string::npos)
      end = stem.size();
    const std::string part = stem.substr(start, end - start);
    if (!part.empty() && !std::ranges::all_of(part, [](unsigned char c) { return std::isdigit(c); }))
      name += (name.empty() ? "" : " ") + part;
    start = end + 1;
  }
  std::ranges::replace(name, '_', ' ');
  entry.title = name.empty() ? entry.kind : Upper(name);
  std::error_code ec;
  const auto time = fs::last_write_time(path, ec);
  entry.when = ec ? std::string() : DescribeTime(time);
  return entry;
}

// Deleting goes through the Recycle Bin where there is one, so a slip can be
// undone.
bool MoveToTrash(const fs::path& path)
{
#if defined(_WIN32)
  std::wstring from = path.wstring();
  from.push_back(L'\0');  // the list ends with a double null
  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = from.c_str();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
#else
  std::error_code ec;
  return fs::remove(path, ec);
#endif
}

// A yes / no question over the page.
struct Confirm
{
  bool open = false;
  std::string title, detail;
  std::function<void()> yes;
  bool fresh = true;  // opened this frame: the press that opened it is not an answer
};

// What can be done with a card (A on the Memory Cards page).
struct CardMenu
{
  bool open = false;
  int index = -1;
  int focus = 0;
  bool fresh = true;  // opened this frame
};

// The BUTTON MAPPING screen, opened from the Controllers page: every
// GameCube input of one player with its binding, rebound by pressing the
// key or button to use.
struct Mapping
{
  bool open = false;
  int port = 0;
  // 0 the player row; 1-12 the left column (kGcInputs 0-11); 13-20 the right
  // column (kGcInputs 12-19); 21 RESET TO DEFAULTS; 22 BACK.
  int focus = 1;
  int capturing = -1;  // the kGcInputs index waiting for input, or -1
  std::chrono::steady_clock::time_point deadline;
  std::vector<bool> keys_down;  // keyboard state when the wait began
  std::string status;
};
constexpr auto kMappingWait = std::chrono::seconds(5);

// The gamepad input ImGui saw pressed this frame, under the name Dolphin's
// SDL backend gives it (face buttons positionally, as the generated profile
// writes them), or empty.
std::string PressedGamepadInput()
{
  static constexpr std::pair<ImGuiKey, const char*> kInputs[] = {
      {ImGuiKey_GamepadFaceDown, "Button A"},   {ImGuiKey_GamepadFaceRight, "Button B"},
      {ImGuiKey_GamepadFaceLeft, "Button X"},   {ImGuiKey_GamepadFaceUp, "Button Y"},
      {ImGuiKey_GamepadStart, "Start"},         {ImGuiKey_GamepadBack, "Back"},
      {ImGuiKey_GamepadL1, "Shoulder L"},       {ImGuiKey_GamepadR1, "Shoulder R"},
      {ImGuiKey_GamepadL2, "Trigger L"},        {ImGuiKey_GamepadR2, "Trigger R"},
      {ImGuiKey_GamepadL3, "Thumb L"},          {ImGuiKey_GamepadR3, "Thumb R"},
      {ImGuiKey_GamepadDpadUp, "Pad N"},        {ImGuiKey_GamepadDpadDown, "Pad S"},
      {ImGuiKey_GamepadDpadLeft, "Pad W"},      {ImGuiKey_GamepadDpadRight, "Pad E"},
      {ImGuiKey_GamepadLStickUp, "Left Y+"},    {ImGuiKey_GamepadLStickDown, "Left Y-"},
      {ImGuiKey_GamepadLStickLeft, "Left X-"},  {ImGuiKey_GamepadLStickRight, "Left X+"},
      {ImGuiKey_GamepadRStickUp, "Right Y+"},   {ImGuiKey_GamepadRStickDown, "Right Y-"},
      {ImGuiKey_GamepadRStickLeft, "Right X-"}, {ImGuiKey_GamepadRStickRight, "Right X+"},
  };
  for (const auto& [key, name] : kInputs)
    if (ImGui::IsKeyPressed(key, false))
      return name;
  return {};
}

// The naming dialog for a new, renamed or copied card.
struct Naming
{
  enum class Mode
  {
    None,
    New,
    Rename,
    Duplicate,
  };
  Mode mode = Mode::None;
  int index = -1;
  std::array<char, 25> text{};  // 24 characters
  bool focus = false;
  bool typed = false;  // the suggestion has been replaced
  moderngekko::onscreen_keyboard::Cursor cursor;
  std::string error;
};

int CardNameFilter(ImGuiInputTextCallbackData* data)
{
  const ImWchar c = data->EventChar;
  return c < 128 && (std::isalnum(c) || c == ' ' || c == '-' || c == '_') ? 0 : 1;
}

// "Play time 42:17" (hours:minutes, as the game shows it), or empty.
std::string CardPlayTime(const moderngekko::frontend::LibraryCard& card)
{
  if (card.playtime < 0)
    return {};
  char text[32];
  std::snprintf(text, sizeof(text), "Play time %d:%02d", card.playtime / 3600,
                card.playtime / 60 % 60);
  return text;
}

std::string CardWhen(const moderngekko::frontend::LibraryCard& card)
{
  if (card.blank)
    return "Blank card";
  return card.saved ? "Saved " + DescribeTime(*card.saved) : std::string();
}

} // namespace


#if defined(_WIN32)
// The runner pins itself to the largest shared L3 when MODERNGEKKO_CACHE_AFFINITY=1,
// and nothing ever set it, so every packaged launch ran unpinned. Measured on the
// seven benchmark scenes, windowed, shipping module, arms differing only by this
// variable: +15.9% mean (95% CI [+13.7%, +18.1%]), 21 of 21 interleaved pairs
// favouring pinned, and it is what takes battle-mid from straddling 60 to holding
// it on every run.
//
// It is set here rather than in the runner's own default because the runner
// cannot know it was launched by us, and a bare `moderngekko-run` invocation --
// every benchmark harness, every developer command line -- must keep its current
// behaviour so old measurements stay comparable.
//
// Gated on DominantSharedCache, not LargestSharedCache: pinning pays only where
// one cache domain is genuinely bigger than the rest. On a part whose dies are
// identical it would halve the available cores and buy nothing, so there it
// declines and the child runs exactly as before.
//
// An explicit setting always wins, in either direction, so anyone measuring can
// force the question.
void EnableCacheAffinityIfDominant()
{
  if (std::getenv("MODERNGEKKO_CACHE_AFFINITY") != nullptr)
    return;

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
  if (length == 0)
    return;
  std::vector<char> buffer(length);
  if (!GetLogicalProcessorInformationEx(
          RelationCache,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
  {
    return;
  }

  if (!moderngekko::frontend::DominantSharedCache(buffer.data(), length))
    return;
  SetEnvironmentVariableA("MODERNGEKKO_CACHE_AFFINITY", "1");
}
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
  // Before any child is spawned: SDL_CreateProcess gives the runner this
  // process's environment.
  EnableCacheAffinityIfDominant();
#endif
  bool use_wayland = false;
  std::optional<fs::path> extract_only;
  // Renders every launcher page once into this directory and exits, so the
  // screens can be checked against the design without driving the window.
  std::optional<fs::path> capture_directory;
  for (int i = 1; i < argc; ++i)
  {
    if (std::string_view(argv[i]) == "-X11" || std::string_view(argv[i]) == "--x11")
      use_wayland = false;
    else if (std::string_view(argv[i]) == "--wayland")
      use_wayland = true;
    else if (std::string_view(argv[i]) == "--extract" && i + 1 < argc)
      extract_only = argv[++i];
    else if (std::string_view(argv[i]) == "--capture-pages" && i + 1 < argc)
      capture_directory = argv[++i];
  }

  const fs::path release_directory = ExecutableDirectory(argv[0]);
  const fs::path user_directory = DefaultUserDirectory(release_directory);
  if (extract_only)
  {
    ExtractionState extraction;
    extraction.running = true;
    const bool success =
        ExtractDisc(*extract_only, user_directory, release_directory, &extraction);
    std::lock_guard lock(extraction.mutex);
    if (!success)
      std::cerr << "extraction failed: " << extraction.error << '\n';
    else
      std::cout << extraction.status << ": " << *extraction.finished_game << '\n';
    return success ? 0 : 1;
  }

  auto config = moderngekko::frontend::LoadConfig(user_directory, true);
  if (!config)
  {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Invalid " MODERNGEKKO_FRONTEND_NAME " config.ini",
                             config.error.c_str(), nullptr);
    return 2;
  }
  moderngekko::CommunityHdTexturePackStatus community_pack_status =
      moderngekko::GetCommunityHdTexturePackStatus(user_directory);
  config.community_hd_texture_pack = community_pack_status.installed;
  std::string community_pack_message;

#if defined(__linux__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, use_wayland ? "wayland" : "x11");
#endif
  SDL_setenv_unsafe("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    return 1;

  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  int window_width = static_cast<int>(1280 * scale), window_height = static_cast<int>(760 * scale);
  SDL_Rect usable{};
  if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0)
  {
    const float fit = std::min({1.0f, usable.w * 0.9f / window_width, usable.h * 0.9f / window_height});
    window_width = static_cast<int>(window_width * fit);
    window_height = static_cast<int>(window_height * fit);
  }
  SDL_Window* window = SDL_CreateWindow(MODERNGEKKO_FRONTEND_NAME, window_width, window_height,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
  {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, "vulkan");
  if (!renderer)
    renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer)
  {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  // Whether "Auto" aspect means native ultrawide on this display.
  std::string display_size = "unknown";
  bool ultrawide_display = false;
  if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window)))
  {
    display_size = std::to_string(mode->w) + "x" + std::to_string(mode->h);
    ultrawide_display = mode->w * 10 >= mode->h * 22;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::vector<fs::path> images = FindDiscImages();
  std::optional<fs::path> selected_image = images.empty() ? std::nullopt : std::optional(images[0]);
  fs::path current_game = ReadDefaultGame(user_directory, release_directory);
  bool current_dol_changed = false;
  std::string current_dol_error;
  auto current_metadata =
      ApplyBundledDolPatch(current_game, release_directory, &current_dol_changed,
                           &current_dol_error) ?
          moderngekko::InspectGame(current_game) :
          moderngekko::GameInspectResult{{}, current_dol_error};
  if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
    current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
  const auto& resolutions = moderngekko::frontend::SupportedResolutions();
  const auto& graphics_backends =
      moderngekko::frontend::SupportedGraphicsBackends();
  bool show_fps_in_title = config.show_fps_in_title;
  bool fullscreen = config.fullscreen;
  std::array<char, 31> netplay_nickname{};
  std::array<char, 256> netplay_address{};
  std::snprintf(netplay_nickname.data(), netplay_nickname.size(), "%s",
                config.netplay_nickname.c_str());
  std::snprintf(netplay_address.data(), netplay_address.size(), "%s",
                config.netplay_address.c_str());
  int netplay_port = config.netplay_port;
  OnlineBattleSettings battle = ReadOnlineBattleSettings(user_directory);
  std::array<char, 256> battle_address{};
  std::snprintf(battle_address.data(), battle_address.size(), "%s", battle.address.c_str());
  std::array<char, 8> battle_port_text{};
  std::snprintf(battle_port_text.data(), battle_port_text.size(), "%d", battle.port);
  bool automatic_buffer = config.netplay_buffer == "auto";
  int manual_buffer = automatic_buffer ? 5 : std::stoi(config.netplay_buffer);
  int resolution_index = 0;
  for (std::size_t i = 0; i < resolutions.size(); ++i)
  {
    if (config.resolution == resolutions[i].text)
      resolution_index = static_cast<int>(i);
  }
  int graphics_backend_index = 0;
  for (std::size_t i = 0; i < graphics_backends.size(); ++i)
  {
    if (config.graphics_backend == graphics_backends[i].value)
      graphics_backend_index = static_cast<int>(i);
  }
  static constexpr std::array aspect_labels = {
      "Auto (game-controlled)", "16:9 widescreen", "4:3 original",
      "32:9 native ultrawide (Hor+)"};
  static constexpr std::array aspect_values = {
      moderngekko::frontend::kFrontendAspectAuto,
      moderngekko::frontend::kFrontendAspectWidescreen,
      moderngekko::frontend::kFrontendAspectStandard,
      moderngekko::frontend::kFrontendAspectUltrawide};
  static constexpr std::array texture_filter_labels = {
      "Game default", "Nearest (sharp pixels)", "Linear (smooth)"};
  static constexpr std::array anisotropy_labels = {
      "Game default", "1x", "2x", "4x", "8x", "16x"};
  static constexpr std::array anisotropy_values = {-1, 0, 1, 2, 3, 4};
  static constexpr std::array navigation_labels = {
      "Off", "Compact minimap", "Debug map + coordinates"};
  static constexpr std::array ultrawide_quality_labels = {
      "3x Performance", "4x Balanced", "5x Quality", "6x Ultra"};
  static constexpr std::array ultrawide_quality_values = {3, 4, 5, 6};
  int aspect_index = 0;
  for (std::size_t i = 0; i < aspect_values.size(); ++i)
  {
    if (config.aspect_ratio == aspect_values[i])
      aspect_index = static_cast<int>(i);
  }
  int texture_filter_index =
      std::clamp(config.force_texture_filtering, 0, 2);
  int anisotropy_index = 0;
  for (std::size_t i = 0; i < anisotropy_values.size(); ++i)
  {
    if (config.max_anisotropy == anisotropy_values[i])
      anisotropy_index = static_cast<int>(i);
  }
  int navigation_index = std::clamp(config.navigation_overlay, 0, 2);
  int ultrawide_quality_index = 0;
  for (std::size_t i = 0; i < ultrawide_quality_values.size(); ++i)
  {
    if (config.ultrawide_efb_scale == ultrawide_quality_values[i])
      ultrawide_quality_index = static_cast<int>(i);
  }
  // StateSaves is Dolphin's own state directory, so states written in-game land
  // here without any extra configuration and show up on the next launch.
  //
  // Listed once at startup rather than every frame, so the dropdown does not
  // hit the filesystem on each redraw. A state written while the launcher is
  // open therefore appears on the next launch, which is the case that matters:
  // states are written by the running game, not by the launcher.
  std::vector<fs::path> launcher_savestates =
      moderngekko::frontend::ListLauncherSavestates(user_directory / "StateSaves");
  // Normal boot keeps ordinary memory-card progression as the default.
  // Loading a savestate must always be an explicit launcher choice.
  int selected_savestate = -1;

  DialogState dialog;
  DialogState gba_rom_dialog, gba_bios_dialog;
  GbaSettings gba = ReadGbaSettings(user_directory);
  std::vector<ControllerOption> controllers = EnumerateControllers();
  bool controller_profile_exists = moderngekko::frontend::ControllerConfigExists(user_directory);
  std::vector<std::string> configured_controllers =
      moderngekko::frontend::ReadConfiguredControllers(user_directory);
  std::string selected_controller =
      configured_controllers.empty() ? config.controller : configured_controllers.front();
  int controller_index = FindController(controllers, selected_controller);
  std::string controller_status;
  const auto select_controller = [&](int index)
  {
    std::string message;
    if (!moderngekko::frontend::GenerateControllerConfig(user_directory, controllers[index].device,
                                                         &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    std::string error;
    if (!moderngekko::frontend::SaveConfig(user_directory, resolutions[resolution_index].text,
                                           config.show_fps_in_title, controllers[index].device,
                                           &error))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(error);
      return false;
    }
    selected_controller = controllers[index].device;
    configured_controllers = {selected_controller};
    config.controller = selected_controller;
    config.controllers = configured_controllers;
    config.resolution = resolutions[resolution_index].text;
    controller_profile_exists = true;
    controller_status = std::move(message);
    return true;
  };
  const auto ensure_controller = [&]
  {
    if (moderngekko::frontend::ControllerConfigExists(user_directory))
    {
      controller_profile_exists = true;
      configured_controllers = moderngekko::frontend::ReadConfiguredControllers(user_directory);
      if (configured_controllers.empty())
      {
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::string(CONTROLLER_PROFILE_NAME) +
                       " has no configured controller device";
        return false;
      }
      selected_controller = configured_controllers.front();
      controller_index = FindController(controllers, selected_controller);
      controller_status = "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
      return true;
    }
    if (controller_index < 0)
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = "Connect an SDL-compatible controller before playing";
      return false;
    }
    return select_controller(controller_index);
  };
  const auto refresh_controllers = [&]
  {
    controllers = EnumerateControllers();
    controller_index = FindController(controllers, selected_controller);
    if (!controller_profile_exists)
    {
      if (controller_index < 0 && !controllers.empty())
      {
        controller_index = 0;
        selected_controller = controllers.front().device;
      }
      if (controller_index >= 0)
        select_controller(controller_index);
      else
        controller_status = "No SDL gamepad detected";
    }
    else
    {
      controller_status = "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
    }
  };
  refresh_controllers();
  ExtractionState extraction;
  std::jthread extraction_thread;
  enum class LaunchMode
  {
    None,
    Solo,
    Host,
    Join,
    BattleHost,  // Colosseum Battle online: an ordinary game plus the link
    BattleJoin,
  };
  bool done = false;
  LaunchMode launch_mode = LaunchMode::None;

  // ---- the themed launcher ----
  // Pokemon Colosseum's own fonts, rebuilt from the player's game files on
  // first run (nothing from the game ships with the launcher).
  const fs::path font_directory = user_directory / "Load" / "ColosseumFont";
  {
    std::string font_error;
    if (!moderngekko::frontend::EnsureColosseumFonts(current_game, font_directory, &font_error))
      SDL_Log("Colosseum fonts unavailable: %s", font_error.c_str());
  }
  const std::string fallback_font =
      (release_directory / "Sys" / "Resources" / "Roboto-Medium.ttf").string();
  theme::Fonts fonts = theme::LoadFonts(font_directory.string() + "/", fallback_font);
  // Until a game is set up the fallback stands in; once one is (browsed to or
  // extracted), its fonts are rebuilt and the launcher switches over.
  fs::path fonts_for_game = theme::HasGameFonts(fonts) ? current_game : fs::path();
  const auto refresh_fonts = [&]
  {
    if (theme::HasGameFonts(fonts) || current_game.empty() || current_game == fonts_for_game)
      return;
    fonts_for_game = current_game;
    std::string font_error;
    if (moderngekko::frontend::EnsureColosseumFonts(current_game, font_directory, &font_error))
      fonts = theme::LoadFonts(font_directory.string() + "/", fallback_font);
  };
  // The Pokemon face portraits (also what the pause menu draws).
  const fs::path icon_directory = user_directory / "Load" / "ColosseumIcons";
  {
    std::string icon_error;
    if (!moderngekko::frontend::EnsureColosseumIcons(current_game, icon_directory, &icon_error))
      SDL_Log("Colosseum portraits unavailable: %s", icon_error.c_str());
  }
  std::string release_version;
  {
    std::ifstream version_file(release_directory / "VERSION");
    std::getline(version_file, release_version);
    while (!release_version.empty() && std::isspace(static_cast<unsigned char>(release_version.back())))
      release_version.pop_back();
  }
  int page = kPlay;
  int capture_frame = 0;
  bool in_page = true;
  std::array<int, kPageCount> row_focus{};
  bool help_open = false;
  // Button hints follow the last input; with a controller connected they start as the
  // mockups show them (A SELECT, B BACK) until the keyboard or mouse is used.
  bool gamepad_input = SDL_HasGamepad();
  std::array<char, 8> netplay_port_text{};
  std::snprintf(netplay_port_text.data(), netplay_port_text.size(), "%d", netplay_port);
  std::vector<StateEntry> state_entries;
  for (const fs::path& path : launcher_savestates)
    state_entries.push_back(DescribeState(path));
  const auto reload_states = [&]
  {
    launcher_savestates =
        moderngekko::frontend::ListLauncherSavestates(user_directory / "StateSaves");
    state_entries.clear();
    for (const fs::path& path : launcher_savestates)
      state_entries.push_back(DescribeState(path));
    selected_savestate = -1;
  };
  // Named memory cards; the inserted one lives in Slot A.
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  const std::string game_id = MODERNGEKKO_REQUIRED_DISC_ID;
#else
  const std::string game_id =
      current_metadata ? current_metadata.metadata->disc_id : std::string("GC6E01");
#endif
  moderngekko::frontend::MemoryCardLibrary card_library(user_directory, game_id);
  {
    std::string card_error;
    if (!card_library.Load(&card_error))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = "Memory cards: " + card_error;
    }
  }
  // Save-state thumbnails (<state>.png, written by the runtime), loaded into
  // textures the first time a row shows them.
  std::unordered_map<std::string, SDL_Texture*> thumbnails;
  const auto png_texture = [&](const std::string& file) -> SDL_Texture*
  {
    const std::string key = file;
    if (const auto it = thumbnails.find(key); it != thumbnails.end())
      return it->second;
    SDL_Texture* texture = nullptr;
    std::string png;
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    if (File::ReadFileToString(key, png) &&
        Common::LoadPNG(std::span(reinterpret_cast<const u8*>(png.data()), png.size()), &rgba, &w,
                        &h) &&
        w && h)
    {
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                  static_cast<int>(w), static_cast<int>(h));
      if (texture)
      {
        SDL_UpdateTexture(texture, nullptr, rgba.data(), static_cast<int>(w * 4));
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
      }
    }
    thumbnails[key] = texture;
    return texture;
  };
  const auto thumbnail = [&](const fs::path& state) { return png_texture(state.string() + ".png"); };
  // Draws species' face as a framed portrait.
  const auto draw_portrait = [&](ImDrawList* dl, const theme::Canvas& cv, int species, float x,
                                 float y, float size, bool active, const std::string& fallback)
  {
    SDL_Texture* atlas = png_texture((icon_directory / "icons_atlas.png").string());
    const int index = moderngekko::frontend::ColosseumIconIndex(species);
    ImVec2 uv0{0, 0}, uv1{0, 0};
    if (atlas && index >= 0)
    {
      float aw = 0, ah = 0;
      SDL_GetTextureSize(atlas, &aw, &ah);
      constexpr float cell = moderngekko::frontend::kColosseumIconCell;
      const float cx = (index % moderngekko::frontend::kColosseumIconColumns) * cell,
                  cy = (index / moderngekko::frontend::kColosseumIconColumns) * cell;
      uv0 = {cx / aw, cy / ah};
      uv1 = {(cx + cell) / aw, (cy + cell) / ah};
    }
    theme::Portrait(dl, cv, fonts,
                    ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(atlas))),
                    atlas && index >= 0, uv0, uv1, x, y, size, active, fallback);
  };
  // Draws a thumbnail into a box, cropped to the box's shape.
  const auto draw_thumbnail = [&](ImDrawList* dl, const theme::Canvas& cv, const fs::path& state,
                                  float x0, float y0, float x1, float y1)
  {
    SDL_Texture* texture = thumbnail(state);
    if (!texture)
      return false;
    float tw = 0, th = 0;
    SDL_GetTextureSize(texture, &tw, &th);
    const float box = (x1 - x0) / (y1 - y0), image = tw / th;
    ImVec2 uv0{0, 0}, uv1{1, 1};
    if (image > box)
      uv0.x = (1 - box / image) / 2, uv1.x = 1 - uv0.x;
    else
      uv0.y = (1 - image / box) / 2, uv1.y = 1 - uv0.y;
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture))),
                 cv.P(x0, y0), cv.P(x1, y1), uv0, uv1);
    return true;
  };
  // The backdrop: the newest save state's picture, blurred (two box passes
  // each way) so it reads as atmosphere rather than detail.
  SDL_Texture* backdrop = nullptr;
  float backdrop_w = 0, backdrop_h = 0;
  for (const StateEntry& entry : state_entries)
  {
    std::string png;
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    if (!File::ReadFileToString(entry.path.string() + ".png", png) ||
        !Common::LoadPNG(std::span(reinterpret_cast<const u8*>(png.data()), png.size()), &rgba, &w,
                         &h) ||
        w < 8 || h < 8)
      continue;
    const int radius = static_cast<int>(std::max(2u, h / 60));
    std::vector<float> a(rgba.begin(), rgba.end()), b(a.size());
    for (int pass = 0; pass < 4; ++pass)
    {
      const bool horizontal = pass % 2 == 0;
      const int n = static_cast<int>(horizontal ? w : h), lines = static_cast<int>(horizontal ? h : w);
      for (int line = 0; line < lines; ++line)
        for (int c = 0; c < 4; ++c)
        {
          const auto at = [&](int i) -> std::size_t {
            const int x = horizontal ? i : line, y = horizontal ? line : i;
            return (static_cast<std::size_t>(y) * w + x) * 4 + c;
          };
          float sum = 0;
          for (int i = -radius; i <= radius; ++i)
            sum += a[at(std::clamp(i, 0, n - 1))];
          for (int i = 0; i < n; ++i)
          {
            b[at(i)] = sum / (2 * radius + 1);
            sum += a[at(std::min(i + radius + 1, n - 1))] - a[at(std::max(i - radius, 0))];
          }
        }
      std::swap(a, b);
    }
    for (std::size_t i = 0; i < rgba.size(); ++i)
      rgba[i] = static_cast<u8>(std::clamp(a[i], 0.0f, 255.0f));
    backdrop = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                 static_cast<int>(w), static_cast<int>(h));
    if (backdrop)
    {
      SDL_UpdateTexture(backdrop, nullptr, rgba.data(), static_cast<int>(w * 4));
      SDL_SetTextureScaleMode(backdrop, SDL_SCALEMODE_LINEAR);
      backdrop_w = static_cast<float>(w);
      backdrop_h = static_cast<float>(h);
    }
    break;
  }
  bool carousel_open = false;
  float carousel_t = 0, carousel_pos = 0;
  int carousel_focus = 0;
  Naming naming;
  Confirm confirm;
  CardMenu card_menu;
  Mapping mapping;

  const auto persist = [&]
  {
    std::string error;
    if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
      return true;
    std::lock_guard lock(dialog.mutex);
    dialog.error = std::move(error);
    return false;
  };
  const auto info = [](std::string text)
  {
    theme::Row row;
    row.kind = theme::Row::Kind::Info;
    row.label = std::move(text);
    row.text_color = theme::color::kGrey;
    return row;
  };
  const auto toggle = [&](const char* label, bool* value, const char* help,
                          bool save = true)
  {
    theme::Row row;
    row.kind = theme::Row::Kind::Toggle;
    row.label = label;
    row.help = help;
    row.on = *value;
    row.changed = [&, value, save](const theme::Row& changed)
    {
      const bool previous = *value;
      *value = changed.on;
      if (!save || persist())
        return true;
      *value = previous;
      return false;
    };
    return row;
  };
  const auto slider = [&](const char* label, int* value, int min, int max, int step,
                          const char* suffix, const char* help, bool enabled = true,
                          bool save = true)
  {
    theme::Row row;
    row.kind = theme::Row::Kind::Slider;
    row.label = label;
    row.help = help;
    row.enabled = enabled;
    row.value = *value;
    row.min = min;
    row.max = max;
    row.step = step;
    row.value_text = std::to_string(*value) + suffix;
    row.changed = [&, value, save](const theme::Row& changed)
    {
      const int previous = *value;
      *value = changed.value;
      if (!save || persist())
        return true;
      *value = previous;
      return false;
    };
    return row;
  };
  const auto choice = [](const char* label, std::vector<std::string> options, int index,
                         const char* help, std::function<bool(int)> apply)
  {
    theme::Row row;
    row.kind = theme::Row::Kind::Choice;
    row.label = label;
    row.help = help;
    row.options = std::move(options);
    row.index = index;
    row.changed = [apply = std::move(apply)](const theme::Row& changed)
    { return apply(changed.index); };
    return row;
  };
  const auto action = [](const char* label, const char* button, const char* help,
                         std::function<void()> run, bool enabled = true)
  {
    theme::Row row;
    row.kind = theme::Row::Kind::Action;
    row.label = label;
    row.options = {button};
    row.help = help;
    row.enabled = enabled;
    row.activated = std::move(run);
    return row;
  };
  const auto text_input = [&](const char* label, char* buffer, std::size_t size, bool numeric,
                              const char* help)
  {
    theme::Row row;
    row.kind = theme::Row::Kind::TextInput;
    row.label = label;
    row.help = help;
    row.buffer = buffer;
    row.buffer_size = size;
    row.numeric = numeric;
    if (numeric)
      row.changed = [&](const theme::Row&)
      {
        netplay_port = std::clamp(std::atoi(netplay_port_text.data()), 1, 65535);
        std::snprintf(netplay_port_text.data(), netplay_port_text.size(), "%d", netplay_port);
        return true;
      };
    return row;
  };
  const auto set_community_pack = [&](bool enabled)
  {
    std::string error;
    if (moderngekko::SetCommunityHdTexturePackEnabled(user_directory, enabled, &error))
    {
      const bool previous_hires = config.hires_textures;
      config.community_hd_texture_pack = enabled;
      if (enabled)
        config.hires_textures = true;
      if (persist())
      {
        community_pack_status = moderngekko::GetCommunityHdTexturePackStatus(user_directory);
        community_pack_message = enabled ? "Community pack installed and enabled." :
                                           "Community pack removed from the active textures.";
        return true;
      }
      std::string rollback_error;
      moderngekko::SetCommunityHdTexturePackEnabled(user_directory, !enabled, &rollback_error);
      config.community_hd_texture_pack = !enabled;
      config.hires_textures = previous_hires;
      return false;
    }
    std::lock_guard lock(dialog.mutex);
    dialog.error = std::move(error);
    return false;
  };
  const auto card_failed = [&](std::string error)
  {
    std::lock_guard lock(dialog.mutex);
    dialog.error = "Memory cards: " + std::move(error);
  };
  const auto insert_card = [&](int index)
  {
    std::string error;
    if (!card_library.Insert(index, &error))
      card_failed(error);
  };
  const auto open_naming = [&](Naming::Mode mode, int index)
  {
    naming.mode = mode;
    naming.index = index;
    naming.focus = true;
    naming.typed = false;
    naming.cursor = {};
    naming.error.clear();
    const std::string initial = mode == Naming::Mode::Rename ?
                                    card_library.Cards()[static_cast<std::size_t>(index)].name :
                                    card_library.SuggestName();
    std::snprintf(naming.text.data(), naming.text.size(), "%s", initial.c_str());
  };
  const auto commit_naming = [&]
  {
    std::string name = naming.text.data();
    while (!name.empty() && name.back() == ' ')
      name.pop_back();
    while (!name.empty() && name.front() == ' ')
      name.erase(name.begin());
    std::string error;
    const bool ok = naming.mode == Naming::Mode::New ? card_library.Create(name, &error) :
                    naming.mode == Naming::Mode::Rename ?
                                                       card_library.Rename(naming.index, name, &error) :
                                                       card_library.Duplicate(naming.index, name, &error);
    if (!ok)
    {
      naming.error = error.empty() ? "Use letters, numbers, spaces, - or _." : error;
      return;
    }
    const auto& cards = card_library.Cards();
    for (std::size_t i = 0; i < cards.size(); ++i)
      if (cards[i].name == name)
      {
        carousel_focus = static_cast<int>(i);
        row_focus[kCards] = static_cast<int>(i);
      }
    naming.mode = Naming::Mode::None;
  };
  // Sets one player's pad (empty: none) and refreshes what the page shows;
  // the other players' sections are left as they are.
  const auto assign_port = [&](int port, const std::string& device)
  {
    std::string message;
    if (!moderngekko::frontend::SetControllerPort(user_directory, port, device, &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return;
    }
    configured_controllers = moderngekko::frontend::ReadConfiguredControllers(user_directory);
    if (!configured_controllers.empty())
    {
      config.controllers = configured_controllers;
      config.controller = configured_controllers.front();
      selected_controller = configured_controllers.front();
      persist();
    }
    controller_index = FindController(controllers, selected_controller);
    controller_profile_exists = true;
    controller_status = std::move(message);
  };
  const auto assign_players = [&](std::vector<std::string> devices)
  {
    std::string message;
    if (!moderngekko::frontend::GenerateControllerConfig(
            user_directory, std::span<const std::string>(devices), &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return;
    }
    const std::vector<std::string> previous = config.controllers;
    config.controllers = devices;
    config.controller = devices.front();
    if (!persist())
    {
      config.controllers = previous;
      return;
    }
    configured_controllers = devices;
    selected_controller = devices.front();
    controller_index = FindController(controllers, selected_controller);
    controller_profile_exists = true;
    controller_status = std::move(message);
  };
  const auto ask_delete_card = [&](int index)
  {
    const auto& cards = card_library.Cards();
    if (index < 0 || index >= static_cast<int>(cards.size()))
      return;
    if (index == card_library.Inserted())
    {
      card_failed("put another card in Slot A before deleting this one");
      return;
    }
    confirm = {true, "DELETE " + Upper(cards[static_cast<std::size_t>(index)].name) + "?",
               "The card and its save go to the Recycle Bin.", [&, index]
               {
                 std::string error;
                 if (!card_library.Delete(index, MoveToTrash, &error))
                   card_failed(error);
               }};
  };
  const auto ask_delete_state = [&](int index)
  {
    if (index < 0 || index >= static_cast<int>(state_entries.size()))
      return;
    const StateEntry entry = state_entries[static_cast<std::size_t>(index)];
    confirm = {true, "DELETE " + entry.title + "?",
               entry.file + " (" + entry.when + ") goes to the Recycle Bin.", [&, entry]
               {
                 if (!MoveToTrash(entry.path))
                 {
                   std::lock_guard lock(dialog.mutex);
                   dialog.error = "Could not delete " + entry.file;
                   return;
                 }
                 // Its picture and camera go with it.
                 for (const char* side : {".png", ".camera"})
                 {
                   std::error_code ec;
                   const fs::path file = fs::path(entry.path).concat(side);
                   if (fs::exists(file, ec))
                     MoveToTrash(file);
                 }
                 reload_states();
               }};
  };
  const auto start_solo = [&](int state)
  {
    selected_savestate = state;
    if (ensure_controller())
    {
      launch_mode = LaunchMode::Solo;
      done = true;
    }
  };
  const auto start_netplay = [&](LaunchMode mode)
  {
    if (!ensure_controller())
      return;
    config.netplay_nickname = netplay_nickname.data();
    config.netplay_address = netplay_address.data();
    config.netplay_port = static_cast<std::uint16_t>(netplay_port);
    config.netplay_buffer = automatic_buffer ? "auto" : std::to_string(manual_buffer);
    config.controllers = configured_controllers.empty() ?
                             std::vector<std::string>{selected_controller} :
                             configured_controllers;
    config.controller = config.controllers.front();
    if (persist())
    {
      launch_mode = mode;
      done = true;
    }
  };
  const auto save_battle = [&]
  {
    battle.address = battle_address.data();
    battle.port = std::clamp(std::atoi(battle_port_text.data()), 1, 65535);
    std::snprintf(battle_port_text.data(), battle_port_text.size(), "%d", battle.port);
    WriteOnlineBattleSettings(user_directory, battle);
    return true;
  };
  const auto start_battle = [&](LaunchMode mode)
  {
    if (!ensure_controller() || !save_battle())
      return;
    selected_savestate = -1;  // a battle starts from the memory card
    launch_mode = mode;
    done = true;
  };
  const auto browse_for_image = [&]
  {
    static constexpr SDL_DialogFileFilter filters[] = {
        {"Disc images", "iso;wbfs;rvz"}, {"ISO", "iso"}, {"WBFS", "wbfs"}, {"RVZ", "rvz"}};
    const std::string documents = DocumentsDirectory().string();
    SDL_ShowOpenFileDialog(FileDialogCallback, &dialog, window, filters,
                           static_cast<int>(std::size(filters)), documents.c_str(), false);
  };
  const auto start_extraction = [&]
  {
    if (!selected_image || extraction.running)
      return;
    extraction.completed = 0;
    extraction.total = 1;
    extraction.running = true;
    {
      std::lock_guard lock(extraction.mutex);
      extraction.error.clear();
      extraction.finished_game.reset();
    }
    const fs::path image = *selected_image;
    extraction_thread = std::jthread(
        [image, user_directory, release_directory, &extraction]
        { ExtractDisc(image, user_directory, release_directory, &extraction); });
  };
  // A game extracted without its native module (by an older release, or the module was
  // removed) is built from the extracted game, once, before it can be played.
  const auto module_missing_for = [&](const moderngekko::GameInspectResult& metadata)
  {
    return metadata && !moderngekko::frontend::HasGameModule(release_directory, user_directory,
                                                              metadata.metadata->disc_id);
  };
  bool module_missing = module_missing_for(current_metadata);
  const auto start_module_build = [&]
  {
    if (!current_metadata || extraction.running)
      return;
    extraction.completed = 0;
    extraction.total = 1;
    extraction.running = true;
    extraction.module_ready = false;
    {
      std::lock_guard lock(extraction.mutex);
      extraction.error.clear();
      extraction.status = "Preparing to build the game";
    }
    const fs::path game = current_game;
    extraction_thread = std::jthread(
        [game, user_directory, release_directory, &extraction]
        {
          std::string error;
          if (EnsureGameModule(game, user_directory, release_directory, &extraction, &error))
          {
            extraction.module_ready = true;
          }
          else
          {
            std::lock_guard lock(extraction.mutex);
            extraction.error = error;
          }
          extraction.running = false;
        });
  };
  if (module_missing && moderngekko::frontend::CanBuildGameModule(release_directory))
    start_module_build();
  while (!done)
  {
    bool controllers_changed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        done = true;
      if (event.type == SDL_EVENT_JOYSTICK_ADDED || event.type == SDL_EVENT_JOYSTICK_REMOVED ||
          event.type == SDL_EVENT_GAMEPAD_REMAPPED)
      {
        controllers_changed = true;
      }
    }
    if (controllers_changed)
      refresh_controllers();

    {
      std::lock_guard lock(dialog.mutex);
      if (dialog.selected)
      {
        selected_image = std::move(dialog.selected);
        dialog.selected.reset();
      }
    }
    for (auto [state, target] : {std::pair{&gba_rom_dialog, &gba.rom},
                                 std::pair{&gba_bios_dialog, &gba.bios}})
    {
      std::lock_guard lock(state->mutex);
      if (state->selected)
      {
        *target = std::move(*state->selected);
        state->selected.reset();
        WriteGbaSettings(user_directory, gba);
      }
    }
    {
      std::lock_guard lock(extraction.mutex);
      if (extraction.finished_game)
      {
        current_game = *extraction.finished_game;
        current_metadata = moderngekko::InspectGame(current_game);
        if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
          current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
        extraction.finished_game.reset();
        module_missing = module_missing_for(current_metadata);
        if (ensure_controller())
        {
          launch_mode = LaunchMode::Solo;
          done = true;
        }
      }
    }
    if (extraction.module_ready.exchange(false))
      module_missing = module_missing_for(current_metadata);
    refresh_fonts();

    // Capture: four frames per shot (fonts bake on first use), pages in order,
    // then the help card open on a display setting and the Slot A card.
    constexpr int kCaptureFrames = 4;
    const int capture_shot = capture_frame / kCaptureFrames;
    constexpr int kCaptureShots = kPageCount + 8;
    if (capture_directory)
    {
      in_page = true;
      help_open = false;
      page = capture_shot < kPageCount ? capture_shot :
             capture_shot == kPageCount ? kDisplay : kPlay;
      if (capture_shot == kPageCount)
      {
        help_open = true;
        row_focus[kDisplay] = 6;
      }
      if (capture_shot == kPageCount + 1)
        row_focus[kPlay] = 1;
      if (capture_shot >= kPageCount + 2)
      {
        if (!carousel_open)
        {
          carousel_focus = std::max(card_library.Inserted(), 0);
          carousel_pos = static_cast<float>(carousel_focus);
        }
        carousel_open = true;
        carousel_t = 1;
      }
      if (capture_shot == kPageCount + 3 && naming.mode == Naming::Mode::None)
        open_naming(Naming::Mode::New, -1);
      if (capture_shot >= kPageCount + 4 && capture_shot < kPageCount + 6)
      {
        carousel_open = false;
        carousel_t = 0;
        naming.mode = Naming::Mode::None;
        page = kCards;
        const int other = card_library.Inserted() == 0 ? 1 : 0;
        if (capture_shot == kPageCount + 4 && !card_menu.open)
          card_menu = {true, other, 0};
        if (capture_shot == kPageCount + 5 && !confirm.open)
        {
          card_menu.open = false;
          ask_delete_card(other);
        }
        if (confirm.open)
          confirm.fresh = true;  // stays up for the picture
      }
      // The button mapping screen, then the same screen waiting for a key.
      if (capture_shot >= kPageCount + 6)
      {
        confirm.open = false;
        card_menu.open = false;
        carousel_open = false;
        carousel_t = 0;
        page = kControllers;
        mapping.open = true;
        mapping.port = 0;
        mapping.focus = capture_shot == kPageCount + 6 ? 1 : 5;
        mapping.capturing = capture_shot == kPageCount + 6 ? -1 : 4;
        mapping.deadline = std::chrono::steady_clock::now() + kMappingWait;
      }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(MODERNGEKKO_FRONTEND_NAME " Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    theme::Canvas cv;
    cv.scale = std::min(viewport->Size.x / theme::kCanvasW, viewport->Size.y / theme::kCanvasH);
    cv.origin = {viewport->Pos.x + (viewport->Size.x - theme::kCanvasW * cv.scale) / 2,
                 viewport->Pos.y + (viewport->Size.y - theme::kCanvasH * cv.scale) / 2};
    const theme::Input raw_in = theme::ReadInput(&gamepad_input);
    // The carousel and the naming dialog sit above the pages and take input.
    const bool overlay = carousel_open || naming.mode != Naming::Mode::None || confirm.open ||
                         card_menu.open || mapping.capturing >= 0;
    const theme::Input in = overlay ? theme::Input{.gamepad = raw_in.gamepad} : raw_in;
    const bool clicked = !overlay && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const auto mouse_in = [&](float x0, float y0, float x1, float y1)
    { return ImGui::IsMouseHoveringRect(cv.P(x0, y0), cv.P(x1, y1), false); };
    const bool mouse_moved =
        !overlay && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0);
    std::string page_hint;

    // ---- frame: title, navigation, title tab, content panel ----
    if (backdrop)
      theme::Backdrop(dl, viewport->Size,
                      ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(backdrop))),
                      backdrop_w, backdrop_h);
    else
      theme::Background(dl, cv, viewport->Size);
    theme::Text(dl, cv, fonts.title, 44, 44, 26, kTitle);
    theme::Text(dl, cv, fonts.menu, 36, 48 + theme::TextWidth(fonts.title, 44, kTitle) + 18, 38,
                "RECOMP", theme::color::kGold);
    {
      const std::string version =
          (current_metadata ? current_metadata.metadata->disc_id : std::string("NO GAME")) +
          (release_version.empty() ? "" : "  v" + release_version);
      theme::Text(dl, cv, fonts.small, 26, 1280 - 44 - theme::TextWidth(fonts.small, 26, version),
                  40, version, IM_COL32(180, 188, 192, 255));
    }
    theme::SteelPanel(dl, cv, 40, 110, 400, 690, 16);
    for (int i = 0; i < kPageCount; ++i)
    {
      const float y = 134 + i * 60;
      if (mouse_in(48, y - 8, 392, y + 52))
      {
        if (mouse_moved && !in_page)
          page = i;
        if (clicked)
        {
          page = i;
          in_page = false;
        }
      }
      const bool active = page == i;
      if (active)
        theme::Chevron(dl, cv, 58, y + 10);
      const ImU32 col = active ? (in_page ? theme::color::kGrey : theme::color::kWhite) :
                        i == kNetplay ? theme::color::kDim : theme::color::kText;
      theme::Text(dl, cv, fonts.menu, 36, 94, y, kPageNames[i], col);
      if (i == kNetplay)
        theme::Text(dl, cv, fonts.body, 13, 96, y + 36, "EXPERIMENTAL", theme::color::kWarn, false);
    }
    theme::SteelPanel(dl, cv, 420, 110, 800, 178, 14);
    theme::Text(dl, cv, fonts.menu, 36, 444, 128,
                page == kControllers && mapping.open ? "BUTTON MAPPING" : kPageNames[page]);
    theme::SteelPanel(dl, cv, 420, 186, 1240, 690, 16);
    if (clicked && mouse_in(420, 186, 1240, 690))
      in_page = true;

    if (!in_page)
    {
      if (in.up)
        page = (page + kPageCount - 1) % kPageCount;
      if (in.down)
        page = (page + 1) % kPageCount;
      if (in.accept || in.right)
        in_page = true;
    }
    else if (in.back)
    {
      // Back on the mapping screen returns to the Controllers page.
      if (page == kControllers && mapping.open)
        mapping.open = false;
      else
        in_page = false;
    }
    if (!in_page || page != kControllers)
    {
      mapping.open = false;
      mapping.capturing = -1;
    }
    // Input for the page this frame (nothing while the navigation has focus).
    const theme::Input page_in = in_page && !in.back ? in : theme::Input{};
    std::string help = kPageHelp[page];

    // ---- pages ----
    std::vector<theme::Row> rows;
    float rows_y = 214, rows_step = 50;
    switch (page)
    {
    case kPlay:
    {
      if (current_metadata && module_missing)
      {
        // One-time build, laid out like the Play page: the big button where PLAY sits, the
        // explanation where the Slot A card text goes, progress underneath.
        const bool building = extraction.running;
        const bool focused = in_page && !building;
        if (!building && ((in_page && page_in.accept) ||
                          (clicked && mouse_in(450, 220, 820, 330))))
          start_module_build();
        theme::SteelPanel(dl, cv, 450, 220, 820, 330, 18,
                          focused ? IM_COL32(110, 26, 30, 235) : theme::color::kPlay);
        const char* label = building ? "BUILDING..." : "BUILD GAME";
        theme::Text(dl, cv, fonts.title, 40, 635 - theme::TextWidth(fonts.title, 40, label) / 2,
                    252, label);
        if (focused)
          theme::Chevron(dl, cv, 478, 258);
        theme::Text(dl, cv, fonts.small, 26, 860, 222, "ONE-TIME SETUP", theme::color::kGold);
        theme::Text(dl, cv, fonts.body, 15, 860, 262, "The game is built from your own disc",
                    theme::color::kGrey, false);
        theme::Text(dl, cv, fonts.body, 15, 860, 286, "on this computer before the first play.",
                    theme::color::kGrey, false);
        theme::Text(dl, cv, fonts.body, 15, 860, 318, "A compiler is downloaded once (about",
                    theme::color::kGrey, false);
        theme::Text(dl, cv, fonts.body, 15, 860, 342, "80 MB); the build takes 5 to 15 minutes.",
                    theme::color::kGrey, false);
        if (building)
        {
          const float progress = std::min(
              1.0f, static_cast<float>(extraction.completed.load()) / extraction.total.load());
          theme::SliderBar(dl, cv, 454, 410, 756, progress);
          std::string step;
          {
            std::lock_guard lock(extraction.mutex);
            step = extraction.status;
          }
          theme::Text(dl, cv, fonts.body, 16, 454, 440, step, theme::color::kGrey, false);
        }
        help = building ? std::string("Building the game. You can leave this page; the build "
                                      "keeps going while the launcher is open.") :
                          std::string("Converts the game's code from your extracted disc and "
                                      "compiles it. Nothing from the game is downloaded.");
        break;
      }
      if (!current_metadata)
      {
        rows.push_back(info("No extracted game is set up yet. Choose your Pokemon Colosseum disc "
                            "image (ISO, RVZ or WBFS)."));
        rows.push_back(info(selected_image ? "Disc: " + selected_image->filename().string() :
                                             std::string("No disc image selected.")));
        rows.push_back(action("DISC IMAGE", "BROWSE",
                              "Pick the disc image to extract. It is read once and unpacked "
                              "next to the launcher; the image itself is not modified.",
                              [&] { browse_for_image(); }, !extraction.running));
        rows.push_back(action("SET UP", "EXTRACT AND PLAY",
                              "Unpacks the game files, checks they match this release, then "
                              "starts the game.",
                              [&] { start_extraction(); },
                              selected_image.has_value() && !extraction.running));
        if (extraction.running)
        {
          const float progress = std::min(
              1.0f, static_cast<float>(extraction.completed.load()) / extraction.total.load());
          theme::SliderBar(dl, cv, 454, 430, 760, progress);
        }
        help = theme::DrawRows(dl, cv, fonts, rows, &row_focus[page], in_page, page_in, 450,
                               rows_y, 410, 62);
        if (help.empty())
          help = kPageHelp[page];
        break;
      }
      // PLAY, the Slot A card, then up to three states to continue from.
      const int states_shown = static_cast<int>(std::min<std::size_t>(3, launcher_savestates.size()));
      const int items = 2 + states_shown;
      int& focus = row_focus[page];
      focus = std::clamp(focus, 0, items - 1);
      if (in_page)
      {
        if (page_in.left && focus == 1)
          focus = 0;
        else if (page_in.right && focus == 0)
          focus = 1;
        else if (page_in.down)
          focus = focus < 2 ? (states_shown ? 2 : focus) : std::min(focus + 1, items - 1);
        else if (page_in.up)
          focus = focus <= 2 ? 0 : focus - 1;
      }
      int activated = page_in.accept ? focus : -1;
      const auto hit = [&](int item, float x0, float y0, float x1, float y1)
      {
        if (!mouse_in(x0, y0, x1, y1))
          return;
        if (mouse_moved)
          focus = item;
        if (clicked)
          activated = item;
      };
      hit(0, 450, 220, 820, 330);
      hit(1, 860, 210, 1024, 400);
      for (int i = 0; i < states_shown; ++i)
        hit(2 + i, 450, 460 + i * 64.0f, 1210, 514 + i * 64.0f);

      theme::SteelPanel(dl, cv, 450, 220, 820, 330, 18,
                        focus == 0 && in_page ? IM_COL32(110, 26, 30, 235) : theme::color::kPlay);
      theme::Text(dl, cv, fonts.title, 44, 635 - theme::TextWidth(fonts.title, 44, "PLAY") / 2,
                  250, "PLAY");
      if (focus == 0 && in_page)
        theme::Chevron(dl, cv, 540, 258);
      {
        const int inserted = card_library.Inserted();
        if (inserted >= 0)
        {
          const auto& card = card_library.Cards()[static_cast<std::size_t>(inserted)];
          theme::MemoryCard(dl, cv, fonts, 860, 210, card.name, theme::CardColor(card.name),
                            focus == 1 && in_page);
          theme::Text(dl, cv, fonts.small, 26, 1046, 222, "SLOT A", theme::color::kGold);
          std::string name = Upper(card.name);
          const float size = std::min(36.0f, 36 * 176 / std::max(1.0f, theme::TextWidth(fonts.menu, 36, name)));
          theme::Text(dl, cv, fonts.menu, size, 1046, 254, name);
          const std::string first =
              card.blank ? std::string("New game") :
              !card.location.empty() ? card.location :
                                       std::to_string(card.blocks) + " blocks";
          const std::string second =
              CardPlayTime(card).empty() ? CardWhen(card) : CardPlayTime(card);
          theme::Text(dl, cv, fonts.body, 13, 1046, 298, first, theme::color::kGrey, false);
          theme::Text(dl, cv, fonts.body, 13, 1046, 320, second, theme::color::kGrey, false);
          if (!CardPlayTime(card).empty())
            theme::Text(dl, cv, fonts.body, 13, 1046, 342, CardWhen(card), theme::color::kDim,
                        false);
        }
        else
        {
          theme::NewCardSlot(dl, cv, fonts, 860, 210, focus == 1 && in_page);
          theme::Text(dl, cv, fonts.small, 26, 1046, 222, "SLOT A", theme::color::kGold);
          theme::Text(dl, cv, fonts.body, 13, 1046, 260, "No card inserted", theme::color::kGrey,
                      false);
        }
      }
      if (states_shown)
        theme::Text(dl, cv, fonts.small, 26, 450, 424, "CONTINUE FROM A SAVE STATE",
                    theme::color::kGold);
      for (int i = 0; i < states_shown; ++i)
      {
        const float y = 460 + i * 64.0f;
        const StateEntry& entry = state_entries[static_cast<std::size_t>(i)];
        theme::SteelPanel(dl, cv, 450, y, 1210, y + 54, 10, theme::color::kPanelDark);
        if (focus == 2 + i && in_page)
          theme::Chevron(dl, cv, 456, y + 18);
        theme::Text(dl, cv, fonts.menu, 36, 478, y + 10, entry.title);
        const bool pictured = draw_thumbnail(dl, cv, entry.path, 1118, y + 9, 1190, y + 45);
        const float when_right = pictured ? 1104 : 1190;
        theme::Text(dl, cv, fonts.small, 26,
                    when_right - theme::TextWidth(fonts.small, 26, entry.when), y + 14, entry.when,
                    theme::color::kGrey);
      }
      if (in_page)
        help = focus == 0 ? "Start the game from the memory card in Slot A, the way the console "
                            "boots." :
               focus == 1 ? "The memory card in Slot A, the one the game loads and saves. "
                            "Press to pick another card." :
                            "Skip the boot and pick up exactly where this save state was made. "
                            "Solo play only.";
      if (activated == 0)
        start_solo(-1);
      else if (activated == 1)
      {
        carousel_open = true;
        carousel_focus = std::max(card_library.Inserted(), 0);
        carousel_pos = static_cast<float>(carousel_focus);
      }
      else if (activated >= 2)
        start_solo(activated - 2);
      break;
    }
    case kCards:
    {
      const auto& cards = card_library.Cards();
      const int count = static_cast<int>(cards.size()) + 1;  // + NEW
      int& focus = row_focus[page];
      focus = std::clamp(focus, 0, count - 1);
      if (page_in.left)
        focus = std::max(focus - 1, 0);
      if (page_in.right)
        focus = std::min(focus + 1, count - 1);
      if (page_in.up && focus >= 4)
        focus -= 4;
      if (page_in.down)
        focus = std::min(focus + 4, count - 1);
      static int first_row = 0;
      first_row = std::clamp(first_row, std::max(0, focus / 4 - 1), focus / 4);
      int activated = page_in.accept ? focus : -1;
      for (int slot = 0; slot < 8; ++slot)
      {
        const int i = first_row * 4 + slot;
        if (i >= count)
          break;
        const float x = 450 + (slot % 4) * 194.0f, y = 210 + (slot / 4) * 232.0f;
        if (mouse_in(x, y, x + 164, y + 190))
        {
          if (mouse_moved)
            focus = i;
          if (clicked)
            activated = i;
        }
        const bool selected = in_page && focus == i;
        if (i == count - 1)
        {
          theme::NewCardSlot(dl, cv, fonts, x, y, selected);
          continue;
        }
        const auto& card = cards[static_cast<std::size_t>(i)];
        theme::MemoryCard(dl, cv, fonts, x, y, card.name, theme::CardColor(card.name), selected);
        if (i == card_library.Inserted())
          theme::Text(dl, cv, fonts.small, 26, x + 82 - theme::TextWidth(fonts.small, 26, "SLOT A") / 2,
                      y + 202, "SLOT A", theme::color::kGold);
      }
      if (focus < count - 1)
      {
        const auto& card = cards[static_cast<std::size_t>(focus)];
        theme::Text(dl, cv, fonts.body, 13, 450, 668,
                    card.name + ".gci   " +
                        (card.blank ? std::string("blank") :
                         !card.location.empty() ? card.location :
                                                  std::to_string(card.blocks) + " blocks") +
                        (CardPlayTime(card).empty() ? "" : "   " + CardPlayTime(card)) + "   " +
                        CardWhen(card),
                    theme::color::kGrey, false);
      }
      else
      {
        theme::Text(dl, cv, fonts.body, 13, 450, 668,
                    "Each card is one .gci save (Colosseum keeps one save per card).",
                    theme::color::kDim, false);
      }
      if (in_page)
      {
        help = focus == count - 1 ?
                   "Make a blank card. Insert it and the game starts a new save on it." :
                   focus == card_library.Inserted() ?
                   "This card is in Slot A: the game loads and saves it. Rename it, or copy it "
                   "to branch your playthrough." :
                   "Insert this card into Slot A. The card that was there keeps its save.";
        page_hint = gamepad_input ? "A OPTIONS    X RENAME    Y COPY    B BACK" :
                                    "ENTER OPTIONS    R RENAME    D COPY    DEL DELETE    ESC BACK";
      }
      if (activated == count - 1)
        open_naming(Naming::Mode::New, -1);
      else if (activated >= 0)
        card_menu = {true, activated, activated == card_library.Inserted() ? 1 : 0};
      else if (in_page && focus < count - 1 && page_in.erase)
        ask_delete_card(focus);
      else if (in_page && focus < count - 1 && page_in.rename)
        open_naming(Naming::Mode::Rename, focus);
      else if (in_page && focus < count - 1 && page_in.duplicate)
        open_naming(Naming::Mode::Duplicate, focus);
      break;
    }
    case kStates:
    {
      int& focus = row_focus[page];
      const int count = static_cast<int>(state_entries.size());
      if (count == 0)
      {
        theme::Text(dl, cv, fonts.menu, 36, 450, 214, "NO SAVE STATES YET", theme::color::kDim);
        theme::Text(dl, cv, fonts.body, 13, 450, 262,
                    "Save one from the pause menu in game, or let room autosaves make them.",
                    theme::color::kDim, false);
        break;
      }
      focus = std::clamp(focus, 0, count - 1);
      if (page_in.down)
        focus = std::min(focus + 1, count - 1);
      if (page_in.up)
        focus = std::max(focus - 1, 0);
      static int first = 0;
      first = std::clamp(first, std::max(0, focus - 4), focus);
      int activated = page_in.accept ? focus : -1;
      for (int row = 0; row < 5 && first + row < count; ++row)
      {
        const int i = first + row;
        const float y = 210 + row * 92.0f;
        if (mouse_in(444, y, 1216, y + 82))
        {
          if (mouse_moved)
            focus = i;
          if (clicked)
            activated = i;
        }
        const StateEntry& entry = state_entries[static_cast<std::size_t>(i)];
        theme::SteelPanel(dl, cv, 444, y, 1216, y + 82, 10, theme::color::kPanelDark);
        dl->AddRectFilled(cv.P(458, y + 10), cv.P(576, y + 72), IM_COL32(30, 40, 46, 255));
        if (!draw_thumbnail(dl, cv, entry.path, 458, y + 10, 576, y + 72))
          theme::Text(dl, cv, fonts.body, 13,
                      517 - theme::TextWidth(fonts.body, 13, "NO PICTURE") / 2, y + 33,
                      "NO PICTURE", theme::color::kDim, false);
        dl->AddRect(cv.P(458, y + 10), cv.P(576, y + 72), IM_COL32(150, 156, 162, 255), 0, 0,
                    cv.S(2));
        if (i == focus && in_page)
          theme::Chevron(dl, cv, 584, y + 31);
        if (entry.title == entry.kind)
        {
          theme::Text(dl, cv, fonts.menu, 36, 612, y + 20, entry.title);
        }
        else
        {
          theme::Text(dl, cv, fonts.menu, 36, 612, y + 8, entry.title);
          theme::Text(dl, cv, fonts.small, 22, 612, y + 50, entry.kind, theme::color::kGold);
        }
        theme::Text(dl, cv, fonts.small, 26, 1196 - theme::TextWidth(fonts.small, 26, entry.when),
                    y + 30, entry.when, theme::color::kGrey);
      }
      if (in_page)
      {
        help = state_entries[static_cast<std::size_t>(focus)].file + ". Loads straight into "
               "this moment instead of booting from the memory card. Solo play only.";
        page_hint = gamepad_input ? "A LOAD    X DELETE    B BACK" :
                                    "ENTER LOAD    DEL DELETE    ESC BACK";
      }
      if (activated >= 0)
        start_solo(activated);
      else if (in_page && (page_in.erase || page_in.rename))
        ask_delete_state(focus);
      break;
    }
    case kDisplay:
    {
      std::vector<std::string> backend_options;
      for (const auto& backend : graphics_backends)
        backend_options.push_back(Upper(backend.text));
      rows.push_back(choice("BACKEND", backend_options, graphics_backend_index,
                            "The graphics API. Vulkan is the tested default; switch only if "
                            "the game fails to start or shows driver problems.",
                            [&](int i)
                            {
                              const std::string previous = config.graphics_backend;
                              config.graphics_backend = graphics_backends[i].value;
                              if (persist())
                              {
                                graphics_backend_index = i;
                                return true;
                              }
                              config.graphics_backend = previous;
                              return false;
                            }));
      const int display_mode = !config.fullscreen ? 0 : config.exclusive_fullscreen ? 2 : 1;
      rows.push_back(choice("DISPLAY MODE", {"WINDOWED", "BORDERLESS", "FULLSCREEN"},
                            display_mode,
                            "Windowed runs in a resizable window. Borderless fills the screen "
                            "as a window, so alt-tab is instant. Fullscreen takes the display "
                            "exclusively at its native refresh rate, which driver frame "
                            "generation such as AMD Fluid Motion Frames needs.",
                            [&](int i)
                            {
                              const bool previous_full = config.fullscreen;
                              const bool previous_exclusive = config.exclusive_fullscreen;
                              config.fullscreen = i > 0;
                              config.exclusive_fullscreen = i == 2;
                              if (persist())
                              {
                                fullscreen = config.fullscreen;
                                return true;
                              }
                              config.fullscreen = previous_full;
                              config.exclusive_fullscreen = previous_exclusive;
                              return false;
                            }));
      std::vector<std::string> resolution_options;
      for (const auto& option : resolutions)
      {
        std::string text = option.text;
        std::ranges::replace(text, 'x', ' ');
        const std::size_t gap = text.find(' ');
        resolution_options.push_back(std::to_string(option.dolphin_scale) + "x  (" +
                                     text.substr(0, gap) + " x " + text.substr(gap + 1) + ")");
      }
      rows.push_back(choice("INTERNAL RESOLUTION", resolution_options, resolution_index,
                            "How many times the console's 640x528 picture is rendered. Higher "
                            "is sharper and costs more GPU time; match it to your screen.",
                            [&](int i)
                            {
                              const std::string previous = config.resolution;
                              config.resolution = resolutions[i].text;
                              if (persist())
                              {
                                resolution_index = i;
                                return true;
                              }
                              config.resolution = previous;
                              return false;
                            }));
      rows.push_back(choice("ASPECT RATIO", {"AUTO", "16:9", "4:3", "32:9 ULTRAWIDE"},
                            aspect_index,
                            "Auto follows your display: it turns on native 32:9 on an "
                            "ultrawide screen. 32:9 widens the camera itself (Hor+), so "
                            "nothing is stretched.",
                            [&](int i)
                            {
                              const int previous = config.aspect_ratio;
                              config.aspect_ratio = aspect_values[i];
                              if (persist())
                              {
                                aspect_index = i;
                                return true;
                              }
                              config.aspect_ratio = previous;
                              return false;
                            }));
      const bool ultrawide = aspect_index == 3 || (aspect_index == 0 && ultrawide_display);
      rows.push_back(info(aspect_index == 0 ?
                              "Your display is " + display_size +
                                  (ultrawide_display ? ", so native ultrawide is on." :
                                                       ", so the game picks 4:3 or 16:9.") :
                              std::string("Aspect is fixed; Auto would follow the display (" +
                                          display_size + ").")));
      if (ultrawide)
      {
        rows.push_back(choice("ULTRAWIDE QUALITY",
                              {"3x PERFORMANCE", "4x BALANCED", "5x QUALITY", "6x ULTRA"},
                              ultrawide_quality_index,
                              "Render scale used only while native ultrawide is active (it "
                              "replaces Internal Resolution there). 5x Quality is about 4K "
                              "of work at 32:9.",
                              [&](int i)
                              {
                                const int previous = config.ultrawide_efb_scale;
                                config.ultrawide_efb_scale = ultrawide_quality_values[i];
                                if (persist())
                                {
                                  ultrawide_quality_index = i;
                                  return true;
                                }
                                config.ultrawide_efb_scale = previous;
                                return false;
                              }));
      }
      rows.push_back(choice("TEXTURE FILTERING", {"GAME DEFAULT", "NEAREST (SHARP)", "LINEAR (SMOOTH)"},
                            texture_filter_index,
                            "How a texture is sampled when it is drawn bigger or smaller than "
                            "it is stored. Game default keeps what Colosseum asks for. Nearest "
                            "keeps hard pixel edges; Linear blends neighbouring pixels so "
                            "surfaces look smooth.",
                            [&](int i)
                            {
                              const int previous = config.force_texture_filtering;
                              config.force_texture_filtering = i;
                              if (persist())
                              {
                                texture_filter_index = i;
                                return true;
                              }
                              config.force_texture_filtering = previous;
                              return false;
                            }));
      std::vector<std::string> anisotropy;
      for (const char* label : anisotropy_labels)
        anisotropy.push_back(Upper(label));
      rows.push_back(choice("ANISOTROPIC FILTERING", anisotropy, anisotropy_index,
                            "Keeps textures sharp when seen at a steep angle: floors, roads "
                            "and walls running away from the camera. Without it they blur "
                            "with distance. 16x is cheap on any modern GPU.",
                            [&](int i)
                            {
                              const int previous = config.max_anisotropy;
                              config.max_anisotropy = anisotropy_values[i];
                              if (persist())
                              {
                                anisotropy_index = i;
                                return true;
                              }
                              config.max_anisotropy = previous;
                              return false;
                            }));
      rows.push_back(toggle("SHOW FPS", &config.show_fps_in_title,
                            "Shows the frame rate in the window title."));
      break;
    }
    case kGameplay:
    {
      rows.push_back(toggle("60 FPS MODE", &config.sixty_fps,
                            "Presents every video field instead of every other one. 60 is the "
                            "ceiling; it roughly doubles the emulation work, so a slower "
                            "machine runs below full speed."));
      rows.push_back(toggle("GUEST IDLE SKIP", &config.guest_idle_skip,
                            "Lets the console sleep through the game's idle loop. This is what "
                            "holds heavy battles at 60; it costs about 8% in the field."));
      rows.push_back(toggle("DUAL CORE", &config.dual_core,
                            "Renders on a second CPU thread. Leave on."));
      rows.push_back(toggle("HOLD SPACE: FAST-FORWARD", &config.fast_forward,
                            "Hold Space to run the game faster, for grinding and long walks."));
      rows.push_back(slider("FAST-FORWARD SPEED", &config.fast_forward_multiplier, 2, 4, 1, "x",
                            "How fast Space runs the game.", config.fast_forward));
      rows.push_back(toggle("ROOM AUTOSAVES", &config.autosave,
                            "Makes a save state each time you change rooms, rotating through "
                            "a few slots. They appear under SAVE STATES."));
      rows.push_back(slider("AUTOSAVE SLOTS", &config.autosave_slots, 1, 10, 1, "",
                            "How many room autosaves to keep before the oldest is reused.",
                            config.autosave));
      rows.push_back(toggle("RESTORE AFTER A STALL", &config.crash_watchdog,
                            "If emulation freezes, reload the newest autosave automatically."));
      rows.push_back(slider("STALL TIMEOUT", &config.crash_watchdog_seconds, 10, 60, 5, " S",
                            "How long the game must be frozen before it is restored.",
                            config.crash_watchdog));
      break;
    }
    case kControllers:
    {
      const int pad_count = static_cast<int>(controllers.size());
      const auto ports = moderngekko::frontend::ReadControllerPorts(user_directory);
      const int p1 = FindController(controllers, ports[0].empty() ? selected_controller : ports[0]);
      const int p2 = FindController(controllers, ports[1]);
      // What plays each player, as the selectors and the mapping screen show it.
      const auto device_label = [&](int i)
      {
        const int assigned = i == 0 ? p1 : p2;
        // A profile device that is not a connected pad (a pad that is
        // unplugged) is shown as the profile has it.
        const std::string& configured = ports[static_cast<std::size_t>(i)];
        return i == 1 && gba.player2 ? std::string("GAME BOY ADVANCE") :
               assigned >= 0 ? Upper(controllers[static_cast<std::size_t>(assigned)].label) :
               moderngekko::frontend::IsKeyboardDevice(configured) ? std::string("KEYBOARD") :
               !configured.empty() ? Upper(configured.substr(configured.rfind('/') + 1)) :
               i == 0 ? std::string("CHOOSE A DEVICE") :
                        std::string("NONE");
      };

      // ---- BUTTON MAPPING: one player's inputs and their bindings ----
      if (mapping.open)
      {
        namespace fe = moderngekko::frontend;
        static_assert(SDL_SCANCODE_A == 4 && SDL_SCANCODE_RETURN == 40 &&
                          SDL_SCANCODE_LSHIFT == 225 && SDL_SCANCODE_UP == 82,
                      "kKeyNames is keyed by HID usage, which SDL scancodes are");
        const int port = std::clamp(mapping.port, 0, 1);
        const std::string device = ports[static_cast<std::size_t>(port)];
        const bool keyboard = fe::IsKeyboardDevice(device);
        const auto bindings = fe::ReadControllerBindings(user_directory, port);
        const auto binding_of = [&](std::string_view key)
        {
          for (const auto& [k, v] : bindings)
            if (k == key)
              return v;
          return std::string();
        };
        const auto bind = [&](int input, const std::string& name)
        {
          const std::string key(fe::kGcInputs[static_cast<std::size_t>(input)].key);
          std::vector<std::pair<std::string, std::string>> changes{{key, fe::QuoteInput(name)}};
          // The analog half of a trigger follows its button.
          if (key == "Triggers/L" || key == "Triggers/R")
            changes.emplace_back(key + "-Analog", fe::QuoteInput(name));
          std::string message;
          if (fe::SetControllerBindings(user_directory, port, changes, &message))
            mapping.status = std::string(fe::kGcInputs[static_cast<std::size_t>(input)].label) +
                             " IS NOW " + fe::ReadableBinding(fe::QuoteInput(name));
          else
            mapping.status = Upper(message);
        };
        const auto now = std::chrono::steady_clock::now();

        // Waiting for the key or button: before the page's own input, so the
        // press that started the wait is not taken as the answer.
        if (mapping.capturing >= 0 && !capture_directory)
        {
          const int input = mapping.capturing;
          const std::string label(fe::kGcInputs[static_cast<std::size_t>(input)].label);
          int count = 0;
          const bool* state = SDL_GetKeyboardState(&count);
          mapping.keys_down.resize(static_cast<std::size_t>(count));
          int pressed_key = -1;
          for (int sc = 0; sc < count; ++sc)
          {
            // Only a key that goes down now: one held when the wait began
            // (Enter) counts once released and pressed again.
            if (state[sc] && !mapping.keys_down[static_cast<std::size_t>(sc)] && pressed_key < 0)
              pressed_key = sc;
            mapping.keys_down[static_cast<std::size_t>(sc)] = state[sc];
          }
          const std::string pad = PressedGamepadInput();
          if (pressed_key == SDL_SCANCODE_ESCAPE || (keyboard && pad == "Button B"))
          {
            mapping.capturing = -1;
            mapping.status = label + " UNCHANGED";
          }
          else if (keyboard && pressed_key >= 0)
          {
            const std::string name = fe::KeyNameForHid(pressed_key, fe::KeyboardBackendOf(device));
            if (name.empty())
            {
              mapping.status = "THAT KEY CANNOT BE BOUND";
            }
            else
            {
              mapping.capturing = -1;
              bind(input, name);
            }
          }
          else if (!keyboard && !pad.empty())
          {
            mapping.capturing = -1;
            bind(input, pad);
          }
          else if (now >= mapping.deadline)
          {
            mapping.capturing = -1;
            mapping.status = "NOTHING PRESSED: " + label + " UNCHANGED";
          }
        }

        int& f = mapping.focus;
        f = std::clamp(f, 0, 22);
        // The right column's rows: eight inputs, a gap, RESET, BACK.
        const auto right_row = [](int item) { return item <= 20 ? item - 13 : item == 21 ? 9 : 10; };
        const auto right_item = [](int row) { return row <= 7 ? 13 + row : row <= 9 ? 21 : 22; };
        if (page_in.up)
          f = f == 0 || f == 1 || f == 13 ? 0 : f - 1;
        else if (page_in.down)
          f = f == 0 ? 1 : f == 12 || f == 22 ? f : f + 1;
        else if (page_in.right && f >= 1 && f <= 12)
          f = right_item(f - 1);
        else if (page_in.left && f >= 13)
          f = 1 + std::min(right_row(f), 11);
        int activate = page_in.accept || (f == 0 && (page_in.left || page_in.right)) ? f : -1;
        const auto hit = [&](int item, float x0, float y0, float x1, float y1)
        {
          if (!mouse_in(x0, y0, x1, y1))
            return;
          if (mouse_moved)
            f = item;
          if (clicked)
            activate = f = item;
        };
        hit(0, 440, 200, 1230, 244);
        for (int i = 0; i < 12; ++i)
          hit(1 + i, 440, 262 + i * 34.0f, 840, 294 + i * 34.0f);
        for (int r = 0; r < 11; ++r)
          if (r != 8)
            hit(right_item(r), 840, 262 + r * 34.0f, 1230, 294 + r * 34.0f);

        if (activate == 0)
        {
          mapping.port = 1 - port;
          mapping.status.clear();
        }
        else if (activate >= 1 && activate <= 20)
        {
          if (device.empty())
          {
            mapping.status = "CHOOSE A DEVICE FOR PLAYER " + std::to_string(port + 1) + " FIRST";
          }
          else
          {
            int count = 0;
            const bool* state = SDL_GetKeyboardState(&count);
            mapping.keys_down.assign(state, state + count);
            mapping.capturing = activate - 1;
            mapping.deadline = now + kMappingWait;
            mapping.status.clear();
          }
        }
        else if (activate == 21 && !device.empty())
        {
          std::string message;
          mapping.status =
              moderngekko::frontend::SetControllerPort(user_directory, port, device, &message) ?
                  std::string("PLAYER ") + std::to_string(port + 1) + " RESET TO THE " +
                      (keyboard ? "KEYBOARD" : "GAMEPAD") + " LAYOUT" :
                  Upper(message);
        }
        else if (activate == 22)
        {
          mapping.open = false;
        }

        // The player, and what they play with.
        if (in_page && f == 0)
          theme::Chevron(dl, cv, 427, 212, 14);
        theme::Text(dl, cv, fonts.small, 24, 450, 208, "PLAYER", theme::color::kGold);
        theme::ValueSelector(dl, cv, fonts, 554, 204, "PLAYER " + std::to_string(port + 1), 220);
        theme::Text(dl, cv, fonts.small, 24, 800, 208, device_label(port),
                    device.empty() ? theme::color::kDim : theme::color::kWhite);
        const auto draw_row = [&](int item, float x, float y)
        {
          const bool focused = in_page && f == item;
          if (focused)
            theme::Chevron(dl, cv, x - 23, y + 4, 14);
          if (item >= 21)
          {
            theme::Text(dl, cv, fonts.small, 24, x, y, item == 21 ? "RESET TO DEFAULTS" : "BACK",
                        focused ? theme::color::kWhite : theme::color::kText);
            return;
          }
          const auto& gc = fe::kGcInputs[static_cast<std::size_t>(item - 1)];
          theme::Text(dl, cv, fonts.small, 24, x, y, std::string(gc.label), theme::color::kGold);
          std::string value;
          ImU32 col = theme::color::kWhite;
          if (mapping.capturing == item - 1)
          {
            const auto left = std::chrono::ceil<std::chrono::seconds>(mapping.deadline - now);
            value = std::string(keyboard ? "PRESS A KEY  " : "PRESS A BUTTON  ") +
                    std::to_string(std::max<long long>(1, left.count()));
            col = theme::color::kWarn;
          }
          else
          {
            value = fe::ReadableBinding(binding_of(gc.key));
            if (value.empty())
              value = "NOT SET", col = theme::color::kDim;
          }
          // The right column's labels (C-STICK DOWN) are the wider ones.
          const bool right = x > 800;
          const float value_x = x + (right ? 166 : 150);
          const float value_width = (right ? 1226 : 836) - value_x;
          const float width = theme::TextWidth(fonts.small, 24, value);
          const float size = width > value_width ? 24 * value_width / width : 24;
          theme::Text(dl, cv, fonts.small, size, value_x, y + (24 - size) / 2, value, col);
        };
        for (int i = 0; i < 12; ++i)
          draw_row(1 + i, 450, 266 + i * 34.0f);
        for (int r = 0; r < 11; ++r)
          if (r != 8)
            draw_row(right_item(r), 850, 266 + r * 34.0f);

        std::string line = mapping.status;
        if (mapping.capturing >= 0)
          line = std::string(keyboard ? "PRESS THE KEY" : "PRESS THE BUTTON") + " FOR " +
                 std::string(fe::kGcInputs[static_cast<std::size_t>(mapping.capturing)].label) +
                 (keyboard ? ".  ESC CANCELS." : ".  WAIT TO CANCEL.");
        theme::Text(dl, cv, fonts.body, 13, 450, 670, line,
                    mapping.capturing >= 0 ? theme::color::kGold : theme::color::kGrey, false);
        page_hint = in.gamepad ? "A BIND    B BACK    Z HELP" : "ENTER BIND    ESC BACK    H HELP";
        if (in_page)
          help = f == 0 ?
                     std::string("Whose buttons to change. Left and right switch between "
                                 "player 1 and player 2.") :
                 f <= 20 ?
                     "Press " + std::string(in.gamepad ? "A" : "Enter") +
                         ", then the key or button to use for " +
                         std::string(fe::kGcInputs[static_cast<std::size_t>(f - 1)].label) +
                         ". Nothing pressed within 5 seconds, or Esc, leaves it as it is." :
                 f == 21 ?
                     std::string("Puts back the default layout for this player's device: W A S "
                                 "D, I J K L and the keys listed in the README on a keyboard, "
                                 "the GameCube positions on a gamepad.") :
                     std::string("Back to the Controllers page.");
        break;
      }

      static constexpr int kFocusOrder[] = {0, 1, 5, 2, 3, 4};
      int& focus = row_focus[page];
      focus = std::clamp(focus, 0, 5);
      {
        const int at = static_cast<int>(std::ranges::find(kFocusOrder, focus) -
                                        std::begin(kFocusOrder));
        if (page_in.down)
          focus = kFocusOrder[std::min(at + 1, 5)];
        if (page_in.up)
          focus = kFocusOrder[std::max(at - 1, 0)];
      }
      int dir = page_in.right || page_in.accept ? 1 : page_in.left ? -1 : 0;
      if (mouse_in(880, 206, 1220, 396))
      {
        if (mouse_moved)
          focus = 5;
        if (clicked)
        {
          focus = 5;
          dir = 1;
        }
      }
      for (int i = 0; i < 2; ++i)
      {
        const float y = 206 + i * 104.0f;
        if (mouse_in(450, y, 860, y + 90))
        {
          if (mouse_moved)
            focus = i;
          if (clicked && mouse_in(554, y + 46, 854, y + 86))
          {
            focus = i;
            dir = mouse_in(554, y + 46, 704, y + 86) ? -1 : 1;
          }
        }
      }
      for (int i = 0; i < 2; ++i)
      {
        const float y = 426 + i * 34.0f;
        if (mouse_in(440, y, 830, y + 32))
        {
          if (mouse_moved)
            focus = 2 + i;
          if (clicked)
          {
            focus = 2 + i;
            dir = 1;
          }
        }
      }
      if (mouse_in(440, 500, 800, 580))
      {
        if (mouse_moved)
          focus = 4;
        if (clicked)
        {
          focus = 4;
          dir = 1;
        }
      }
      if (dir && focus == 0 && pad_count > 0)
      {
        int next = p1 < 0 ? 0 : ((p1 + dir) % pad_count + pad_count) % pad_count;
        if (next == p2 && pad_count > 1)
          next = ((next + dir) % pad_count + pad_count) % pad_count;
        if (next != p2)
          assign_port(0, controllers[static_cast<std::size_t>(next)].device);
      }
      else if (dir && focus == 1)
      {
        // Options: none, every pad except player 1's, then a Game Boy Advance.
        const int options = pad_count + 2;
        int option = gba.player2 ? pad_count + 1 : p2 + 1;
        do
          option = ((option + dir) % options + options) % options;
        while (option > 0 && option <= pad_count && option - 1 == p1);
        const bool to_gba = option == pad_count + 1;
        if (to_gba != gba.player2)
        {
          gba.player2 = to_gba;
          WriteGbaSettings(user_directory, gba);
        }
        if (to_gba)
        {
          // The GBA is played on the first pad that is not player 1's.
          std::string pad;
          for (int i = 0; i < pad_count && pad.empty(); ++i)
            if (i != p1 && !moderngekko::frontend::IsKeyboardDevice(
                               controllers[static_cast<std::size_t>(i)].device))
              pad = controllers[static_cast<std::size_t>(i)].device;
          if (pad.empty() && p1 >= 0 &&
              !moderngekko::frontend::IsKeyboardDevice(
                  controllers[static_cast<std::size_t>(p1)].device))
            pad = controllers[static_cast<std::size_t>(p1)].device;
          if (!pad.empty())
            MapGbaPad(user_directory, pad);
          assign_port(1, std::string());
        }
        else
        {
          assign_port(1, option == 0 ? std::string() :
                                       controllers[static_cast<std::size_t>(option - 1)].device);
        }
      }
      else if (dir && (focus == 2 || focus == 3))
      {
        const bool rom = focus == 2;
        static constexpr SDL_DialogFileFilter rom_filters[] = {{"Game Boy Advance games", "gba"}};
        static constexpr SDL_DialogFileFilter bios_filters[] = {{"GBA BIOS", "bin;rom"}};
        const fs::path& current = rom ? gba.rom : gba.bios;
        const std::string start = current.empty() ? DocumentsDirectory().string() :
                                                    current.parent_path().string();
        SDL_ShowOpenFileDialog(FileDialogCallback, rom ? &gba_rom_dialog : &gba_bios_dialog,
                               window, rom ? rom_filters : bios_filters, 1, start.c_str(),
                               false);
      }
      else if (dir && focus == 4)
      {
        config.input_overlay = !config.input_overlay;
        if (!persist())
          config.input_overlay = !config.input_overlay;
      }
      else if (dir > 0 && focus == 5)
      {
        mapping.open = true;
        mapping.focus = 1;
        mapping.capturing = -1;
        mapping.status.clear();
      }
      // The mapping screen opens on the player last chosen here.
      if (focus == 0 || focus == 1)
        mapping.port = focus;

      // Espeon and Umbreon, the two partners the game starts you with.
      constexpr int kPlayerSpecies[2] = {196, 197};
      for (int i = 0; i < 2; ++i)
      {
        const float y = 206 + i * 104.0f;
        draw_portrait(dl, cv, kPlayerSpecies[i], 450, y, 64, in_page && focus == i,
                      "P" + std::to_string(i + 1));
        theme::Text(dl, cv, fonts.menu, 36, 554, y + 8, "PLAYER " + std::to_string(i + 1));
        theme::ValueSelector(dl, cv, fonts, 554, y + 48, device_label(i), 300, pad_count > 0);
      }
      // The way into BUTTON MAPPING, with what each player has.
      theme::SteelPanel(dl, cv, 880, 206, 1220, 396, 14, IM_COL32(8, 10, 12, 230));
      if (in_page && focus == 5)
        theme::Chevron(dl, cv, 896, 228, 14);
      theme::Text(dl, cv, fonts.menu, 36, 920, 222, "BUTTON MAPPING",
                  in_page && focus == 5 ? theme::color::kWhite : theme::color::kText);
      for (int i = 0; i < 2; ++i)
      {
        theme::Text(dl, cv, fonts.small, 24, 920, 280 + i * 34.0f, "P" + std::to_string(i + 1),
                    theme::color::kGold);
        std::string label = device_label(i);
        // Shortened to fit the panel, a letter at a time.
        while (label.size() > 4 && theme::TextWidth(fonts.small, 24, label) > 236)
          label = label.substr(0, label.size() - 4) + "...";
        theme::Text(dl, cv, fonts.small, 24, 964, 280 + i * 34.0f, label, theme::color::kWhite);
      }
      theme::Text(dl, cv, fonts.body, 13, 920, 360,
                  gamepad_input ? "A: CHANGE ANY BUTTON OR STICK" :
                                  "ENTER: CHANGE ANY KEY OR BUTTON",
                  theme::color::kDim, false);
      for (int i = 0; i < 2; ++i)
      {
        const float y = 430 + i * 34.0f;
        const fs::path& file = i == 0 ? gba.rom : gba.bios;
        if (in_page && focus == 2 + i)
          theme::Chevron(dl, cv, 427, y + 4, 14);
        theme::Text(dl, cv, fonts.small, 24, 450, y, i == 0 ? "GBA GAME" : "GBA BIOS",
                    theme::color::kGold);
        std::string name = file.empty() ? std::string("CHOOSE...") : Upper(file.stem().string());
        if (name.size() > 26)
          name = name.substr(0, 25) + "...";
        theme::Text(dl, cv, fonts.small, 24, 590, y, name,
                    file.empty() ? theme::color::kDim : theme::color::kWhite);
      }
      theme::Text(dl, cv, fonts.small, 26, 450, 506, "INPUT OVERLAY", theme::color::kGold);
      if (in_page && focus == 4)
        theme::Chevron(dl, cv, 427, 510, 14);
      theme::PokeBall(dl, cv, 454, 544, config.input_overlay);
      theme::Text(dl, cv, fonts.menu, 36, 500, 540, config.input_overlay ? "ON" : "OFF",
                  config.input_overlay ? theme::color::kWhite : theme::color::kDim);
      theme::Text(dl, cv, fonts.body, 13, 450, 588, "Draws the controller on screen and",
                  theme::color::kDim, false);
      theme::Text(dl, cv, fonts.body, 13, 450, 610, "lights each button as it is pressed.",
                  theme::color::kDim, false);
      theme::Text(dl, cv, fonts.body, 13, 450, 656, controller_status, theme::color::kGrey,
                  false);

      theme::SteelPanel(dl, cv, 840, 420, 1220, 676, 14, IM_COL32(8, 10, 12, 230));
      ImVec2 stick, cstick;
      unsigned pressed = theme::GamepadButtons(&stick, &cstick);
      if (capture_directory)
      {
        // A sample press, so the captured page shows the lit state.
        pressed = theme::kGcA | theme::kGcR | theme::kGcUp;
        stick = {0.7f, -0.4f};
      }
      const char* caption = pressed ? "PREVIEW" : "PREVIEW  (PRESS ANY BUTTON)";
      theme::Text(dl, cv, fonts.body, 13, 1030 - theme::TextWidth(fonts.body, 13, caption) / 2,
                  434, caption, theme::color::kGrey, false);
      theme::GcController(dl, cv, 1030, 552, 300, pressed, stick, cstick);
      if (in_page)
        help = focus == 0 ? "The gamepad or the keyboard for player 1. Changing it writes "
                            "that device's default GameCube layout; BUTTON MAPPING changes "
                            "single buttons." :
               focus == 1 ? "A second gamepad or the keyboard for player 2, for two-player "
                            "battles, or a GAME BOY ADVANCE for Colosseum's GBA battles. NONE "
                            "leaves port 2 empty. Player 1's setup is not touched." :
               focus == 5 ? "Choose which key or button plays each GameCube button, stick "
                            "direction and trigger, for player 1 or player 2." :
               focus == 2 ? "The GBA game that Colosseum links with: Pokemon Ruby or "
                            "Sapphire. Each GBA keeps its own save; save at a Pokemon "
                            "Center on it before linking." :
               focus == 3 ? "A GBA BIOS dumped from your own Game Boy Advance. Colosseum "
                            "boots each GBA over the link cable, which needs it." :
                            "Draws the controller on screen in game and lights each button as "
                            "it is pressed.";
      break;
    }
    case kMods:
    {
      rows.push_back(choice("NAVIGATION OVERLAY", {"OFF", "COMPACT MINIMAP", "DEBUG MAP"},
                            navigation_index,
                            "A minimap of the current area. Debug map adds room numbers and "
                            "coordinates.",
                            [&](int i)
                            {
                              const int previous = config.navigation_overlay;
                              config.navigation_overlay = i;
                              if (persist())
                              {
                                navigation_index = i;
                                return true;
                              }
                              config.navigation_overlay = previous;
                              return false;
                            }));
      {
        theme::Row contrast = toggle("HIGH-CONTRAST MINIMAP", &config.minimap_high_contrast,
                                     "Stronger minimap colours. Turning it on also starts the "
                                     "compact minimap if the overlay is off.");
        contrast.changed = [&](const theme::Row& row)
        {
          const bool previous = config.minimap_high_contrast;
          const int previous_overlay = config.navigation_overlay;
          config.minimap_high_contrast = row.on;
          config.navigation_overlay =
              moderngekko::automation::ResolveColosseumNavigationOverlayMode(
                  config.navigation_overlay, config.minimap_high_contrast);
          if (persist())
          {
            navigation_index = config.navigation_overlay;
            return true;
          }
          config.minimap_high_contrast = previous;
          config.navigation_overlay = previous_overlay;
          return false;
        };
        rows.push_back(contrast);
      }
      rows.push_back(slider("OVERLAY TEXT SIZE", &config.osd_font_size, 8, 48, 1, "",
                            "Text size of on-screen messages and overlays."));
      rows.push_back(slider("OVERLAY UI SCALE", &config.accessibility_ui_scale, 75, 200, 5, "%",
                            "Scales the overlays (minimap, input display) for readability."));
      rows.push_back(toggle("HD TEXTURES", &config.hires_textures,
                            "Loads replacement textures from Load/Textures."));
      rows.push_back(toggle("HD DYNAMIC TEXT", &config.text_upscale,
                            "Rebuilds the game's dialogue and menu text at 4x on the GPU."));
      {
        theme::Row pack = toggle("COMMUNITY HD PACK (SEPHIE)", &config.community_hd_texture_pack,
                                 community_pack_status.available ?
                                     "Sephie's community texture pack, installed into the "
                                     "active texture folder. ModernGekko's own UI fixes take "
                                     "priority over it." :
                                     "Not installed: put Pokemon Colosseum HD Pack.zip in "
                                     "User/ResourcePacks, then turn this on.");
        pack.enabled = community_pack_status.available;
        pack.changed = [&](const theme::Row& row) { return set_community_pack(row.on); };
        rows.push_back(pack);
      }
      rows.push_back(info(community_pack_message.empty() ?
                              (community_pack_status.available ?
                                   std::to_string(community_pack_status.texture_count) +
                                       " community textures available." :
                                   std::string("HD textures and HD text are on by default.")) :
                              community_pack_message));
      break;
    }
    case kNetplay:
    {
      rows_y = 250;
      rows_step = 56;
      theme::Text(dl, cv, fonts.body, 13, 450, 214,
                  "Still being tested: expect desyncs. Both players need the same version and "
                  "the same card.",
                  theme::color::kWarn, false);
      rows.push_back(text_input("NICKNAME", netplay_nickname.data(), netplay_nickname.size(),
                                false, "The name other players see."));
      rows.push_back(text_input("HOST / IP", netplay_address.data(), netplay_address.size(),
                                false, "The host's address, for joining. Not needed to host."));
      rows.push_back(text_input("UDP PORT", netplay_port_text.data(), netplay_port_text.size(),
                                true, "The UDP port the host listens on (default 2626). The "
                                      "host must allow it through their firewall."));
      rows.push_back(toggle("AUTOMATIC INPUT BUFFER", &automatic_buffer,
                            "Picks the input delay from the measured connection.", false));
      rows.push_back(slider("BUFFER FRAMES", &manual_buffer, 1, 20, 1, "",
                            "Fixed input delay in frames. Higher hides more lag.",
                            !automatic_buffer, false));
      rows.push_back(action("START", "HOST",
                            "Open a session on this machine and wait for players to join.",
                            [&] { start_netplay(LaunchMode::Host); }));
      rows.push_back(action("CONNECT", "JOIN",
                            "Connect to the host address above.",
                            [&] { start_netplay(LaunchMode::Join); }));
      break;
    }
    case kOnlineBattle:
    {
      rows_y = 250;
      rows_step = 56;
      theme::Text(dl, cv, fonts.body, 13, 450, 214,
                  "Each player uses their own save. In the game: Colosseum Battle, Gang Battle, "
                  "two controllers.",
                  theme::color::kGrey, false);
      theme::Row address = text_input("HOST / IP", battle_address.data(), battle_address.size(),
                                      false, "The host's address, for joining.");
      address.changed = [&](const theme::Row&) { return save_battle(); };
      rows.push_back(std::move(address));
      theme::Row port = text_input("TCP PORT", battle_port_text.data(), battle_port_text.size(),
                                   false, "The TCP port the host listens on (default 27100). The "
                                          "host must allow it through their firewall.");
      port.changed = [&](const theme::Row&) { return save_battle(); };
      rows.push_back(std::move(port));
      rows.push_back(choice("HOST PLAYS AS", {"PLAYER 1", "PLAYER 2"}, battle.seat - 1,
                            "Which player the host is. The joining player is the other.",
                            [&](int index)
                            {
                              battle.seat = index + 1;
                              return save_battle();
                            }));
      static constexpr std::array<int, 4> kTimers{0, 30, 60, 120};
      const auto timer = std::find(kTimers.begin(), kTimers.end(), battle.turn_seconds);
      rows.push_back(choice("TURN TIMER", {"OFF", "30 SECONDS", "1 MINUTE", "2 MINUTES"},
                            timer == kTimers.end() ? 0 : static_cast<int>(timer - kTimers.begin()),
                            "Time for each turn's choice. Player 1's setting applies to both.",
                            [&](int index)
                            {
                              battle.turn_seconds = kTimers[static_cast<std::size_t>(index)];
                              return save_battle();
                            }));
      rows.push_back(choice("WHEN TIME RUNS OUT", {"CHOOSE FOR ME", "GIVE UP"},
                            battle.auto_choose ? 0 : 1,
                            "What happens to a player whose turn timer runs out.",
                            [&](int index)
                            {
                              battle.auto_choose = index == 0;
                              return save_battle();
                            }));
      rows.push_back(action("START", "HOST BATTLE",
                            "Start the game and wait for the other player to connect.",
                            [&] { start_battle(LaunchMode::BattleHost); }));
      rows.push_back(action("CONNECT", "JOIN BATTLE",
                            "Start the game and connect to the host address above.",
                            [&] { start_battle(LaunchMode::BattleJoin); }));
      break;
    }
    }
    // The Play page's set-up states draw their own rows.
    if (!rows.empty() && !(page == kPlay && (!current_metadata || module_missing)))
    {
      const std::string row_help = theme::DrawRows(dl, cv, fonts, rows, &row_focus[page],
                                                   in_page, page_in, 450, rows_y, 410, rows_step);
      if (in_page && !row_help.empty())
        help = row_help;
    }

    // ---- help: the top-right corner, H on a keyboard or Z on a controller ----
    if (in.help)
      help_open = !help_open;
    {
      const float x0 = 816, y0 = 110, x1 = 1240;
      const float wrap = x1 - x0 - 36;
      if (help_open)
      {
        const float height = theme::Paragraph(nullptr, cv, fonts.body, 13, 0, 0, wrap, help,
                                              0, 22);
        const float y1 = std::max(178.0f, y0 + 50 + height + 12);
        theme::SteelPanel(dl, cv, x0, y0, x1, y1, 14, IM_COL32(12, 20, 19, 245));
        theme::Text(dl, cv, fonts.small, 22, x0 + 20, y0 + 16, "ABOUT", theme::color::kGold);
        theme::Paragraph(dl, cv, fonts.body, 13, x0 + 20, y0 + 50, wrap, help,
                         theme::color::kText, 22);
      }
      else
      {
        theme::SteelPanel(dl, cv, x0, y0, x1, 178, 14);
        const std::string key = in.gamepad ? "Z" : "H";
        theme::Text(dl, cv, fonts.menu, 36, x0 + 22, 128, key, theme::color::kGold);
        theme::Text(dl, cv, fonts.small, 26, x0 + 58, 134, "ABOUT THIS OPTION",
                    theme::color::kText);
      }
      if (clicked && mouse_in(x0, y0, x1, 178))
        help_open = !help_open;
    }

    // ---- status line and button hints ----
    {
      std::string status;
      ImU32 status_color = theme::color::kDim;
      {
        std::lock_guard lock(extraction.mutex);
        if (!extraction.error.empty())
          status = extraction.error, status_color = theme::color::kWarn;
        else if (extraction.running || !extraction.status.empty())
          status = extraction.status;
      }
      {
        std::lock_guard lock(dialog.mutex);
        if (!dialog.error.empty())
          status = dialog.error, status_color = theme::color::kWarn;
      }
      if (!status.empty())
        theme::Text(dl, cv, fonts.body, 13, 44, 712, status, status_color, false);
      const std::string hint = !page_hint.empty() ? page_hint :
                               in.gamepad ? "A SELECT    B BACK    Z HELP" :
                                            "ENTER SELECT    ESC BACK    H HELP";
      if (!overlay)
        theme::Text(dl, cv, fonts.small, 26, 1280 - 60 - theme::TextWidth(fonts.small, 26, hint),
                    706, hint, theme::color::kText);
    }

    // ---- Slot A carousel: the cards on a ring, the chosen one in front ----
    {
      const float dt = ImGui::GetIO().DeltaTime;
      if (!capture_directory)
        carousel_t = std::clamp(carousel_t + (carousel_open ? dt : -dt) * 5, 0.0f, 1.0f);
      if (carousel_t > 0)
      {
        const auto& cards = card_library.Cards();
        const int count = static_cast<int>(cards.size()) + 1;  // + NEW
        carousel_focus = std::clamp(carousel_focus, 0, count - 1);
        const bool active = carousel_open && naming.mode == Naming::Mode::None && !confirm.open &&
                            !card_menu.open;
        if (active)
        {
          const float wheel = ImGui::GetIO().MouseWheel;
          if (raw_in.left || wheel > 0)
            carousel_focus = (carousel_focus + count - 1) % count;
          if (raw_in.right || wheel < 0)
            carousel_focus = (carousel_focus + 1) % count;
          bool accept = raw_in.accept;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
          {
            const float mx = (ImGui::GetIO().MousePos.x - cv.origin.x) / cv.scale;
            const float my = (ImGui::GetIO().MousePos.y - cv.origin.y) / cv.scale;
            if (std::abs(mx - 640) < 132 && std::abs(my - 330) < 152)
              accept = true;
            else if (my < 640)
              carousel_focus = (carousel_focus + (mx < 640 ? count - 1 : 1)) % count;
          }
          if (raw_in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            carousel_open = false;
          else if (accept && carousel_focus == count - 1)
            open_naming(Naming::Mode::New, -1);
          else if (accept)
          {
            insert_card(carousel_focus);
            carousel_open = false;
          }
          else if (raw_in.rename && carousel_focus < count - 1)
            open_naming(Naming::Mode::Rename, carousel_focus);
          else if (raw_in.duplicate && carousel_focus < count - 1)
            open_naming(Naming::Mode::Duplicate, carousel_focus);
        }
        // Spin towards the chosen card; the ring wraps when it is full.
        constexpr float pi = 3.14159265f;
        const bool ring = count >= 12;
        const float spacing = ring ? 2 * pi / count : 0.62f;
        float target = static_cast<float>(carousel_focus);
        if (ring)
        {
          while (target - carousel_pos > count / 2.0f)
            target -= count;
          while (carousel_pos - target > count / 2.0f)
            target += count;
        }
        carousel_pos += (target - carousel_pos) * std::min(1.0f, dt * 9);
        if (capture_directory)
          carousel_pos = target;
        if (ring)
          carousel_pos = std::fmod(std::fmod(carousel_pos, static_cast<float>(count)) + count,
                                   static_cast<float>(count));

        theme::Starfield(dl, cv, viewport->Size, carousel_t, static_cast<float>(ImGui::GetTime()));
        const auto fade = [&](ImU32 col)
        {
          const ImU32 a = static_cast<ImU32>(((col >> IM_COL32_A_SHIFT) & 0xFF) * carousel_t);
          return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
        };
        const char* heading = "CHOOSE A MEMORY CARD";
        theme::Text(dl, cv, fonts.title, 44, 640 - theme::TextWidth(fonts.title, 44, heading) / 2,
                    54, heading, fade(theme::color::kWhite));

        struct Placed
        {
          int index;
          float theta, depth;
        };
        std::vector<Placed> placed;
        for (int i = 0; i < count; ++i)
        {
          float theta = (i - carousel_pos) * spacing;
          if (ring)
            theta = std::remainder(theta, 2 * pi);
          if (std::abs(theta) > 2.6f)
            continue;
          placed.push_back({i, theta, std::cos(theta)});
        }
        std::ranges::sort(placed, {}, &Placed::depth);
        for (const Placed& item : placed)
        {
          const float front = (item.depth + 1) / 2;  // 0 at the back, 1 in front
          const float s = 0.45f + 1.15f * front * front * front;
          const float x = 640 + std::sin(item.theta) * 440;
          const float y = 330 - (1 - item.depth) * 70;
          theme::Canvas card_canvas;
          card_canvas.scale = cv.scale * s;
          card_canvas.origin = cv.P(x - 82 * s, y - 95 * s);
          const int start = dl->VtxBuffer.Size;
          const bool chosen = item.index == carousel_focus;
          if (item.index == count - 1)
            theme::NewCardSlot(dl, card_canvas, fonts, 0, 0, chosen);
          else
          {
            const auto& card = cards[static_cast<std::size_t>(item.index)];
            theme::MemoryCard(dl, card_canvas, fonts, 0, 0, card.name, theme::CardColor(card.name),
                              chosen);
          }
          theme::TransformSince(dl, start, cv.P(x, 0).x,
                                0.35f + 0.65f * std::abs(std::cos(item.theta)),
                                carousel_t * (0.15f + 0.85f * front * front * front * front),
                                0.35f + 0.65f * front);
        }

        // The chosen card's details under the ring.
        if (carousel_focus == count - 1)
        {
          const char* text = "NEW CARD";
          theme::Text(dl, cv, fonts.title, 44, 640 - theme::TextWidth(fonts.title, 44, text) / 2,
                      520, text, fade(theme::color::kWhite));
          const char* sub = "A blank card: the game starts a new save on it.";
          theme::Text(dl, cv, fonts.body, 13, 640 - theme::TextWidth(fonts.body, 13, sub) / 2, 580,
                      sub, fade(theme::color::kGrey), false);
        }
        else
        {
          const auto& card = cards[static_cast<std::size_t>(carousel_focus)];
          const std::string name = Upper(card.name);
          theme::Text(dl, cv, fonts.title, 44, 640 - theme::TextWidth(fonts.title, 44, name) / 2,
                      520, name, fade(theme::color::kWhite));
          const std::string where =
              carousel_focus == card_library.Inserted() ? "IN SLOT A" : "IN THE LIBRARY";
          theme::Text(dl, cv, fonts.small, 26, 640 - theme::TextWidth(fonts.small, 26, where) / 2,
                      574, where, fade(theme::color::kGold));
          const std::string detail =
              (card.blank ? std::string("Blank") :
               !card.location.empty() ? card.location :
                                        std::to_string(card.blocks) + " blocks") +
              (CardPlayTime(card).empty() ? "" : "     " + CardPlayTime(card)) + "     " +
              CardWhen(card);
          theme::Text(dl, cv, fonts.body, 13, 640 - theme::TextWidth(fonts.body, 13, detail) / 2,
                      612, detail, fade(theme::color::kGrey), false);
        }
        const std::string hint = gamepad_input ?
                                     "< > CHOOSE    A INSERT    X RENAME    Y COPY    B BACK" :
                                     "< > CHOOSE    ENTER INSERT    R RENAME    D COPY    ESC BACK";
        theme::Text(dl, cv, fonts.small, 26, 640 - theme::TextWidth(fonts.small, 26, hint) / 2, 690,
                    hint, fade(theme::color::kText));
      }
    }

    // ---- card options ----
    if (card_menu.open && naming.mode == Naming::Mode::None && !confirm.open)
    {
      const auto& cards = card_library.Cards();
      if (card_menu.index < 0 || card_menu.index >= static_cast<int>(cards.size()))
        card_menu.open = false;
      else
      {
        const bool inserted = card_menu.index == card_library.Inserted();
        struct Item
        {
          const char* label;
          bool enabled;
        };
        const Item items[] = {{"PUT IN SLOT A", !inserted}, {"RENAME", true}, {"COPY", true},
                              {"DELETE", !inserted}, {"CANCEL", true}};
        constexpr int n = static_cast<int>(std::size(items));
        dl->AddRectFilled({0, 0}, viewport->Size, IM_COL32(0, 0, 0, 140));
        const float x0 = 470, x1 = 810, y0 = 210, step = 56;
        const float y1 = y0 + 78 + n * step + 10;
        theme::SteelPanel(dl, cv, x0, y0, x1, y1, 16, IM_COL32(12, 20, 19, 245));
        const std::string title = Upper(cards[static_cast<std::size_t>(card_menu.index)].name);
        const float title_size =
            std::min(36.0f, 36 * 280 / std::max(1.0f, theme::TextWidth(fonts.menu, 36, title)));
        theme::Text(dl, cv, fonts.menu, title_size, x0 + 30, y0 + 22 + (36 - title_size) / 2, title,
                    theme::color::kGold);
        int choose = -1;
        if (raw_in.up)
          do
            card_menu.focus = (card_menu.focus + n - 1) % n;
          while (!items[card_menu.focus].enabled);
        if (raw_in.down)
          do
            card_menu.focus = (card_menu.focus + 1) % n;
          while (!items[card_menu.focus].enabled);
        if (!items[card_menu.focus].enabled)
          card_menu.focus = 1;
        for (int i = 0; i < n; ++i)
        {
          const float y = y0 + 78 + i * step;
          if (items[i].enabled &&
              ImGui::IsMouseHoveringRect(cv.P(x0 + 20, y - 6), cv.P(x1 - 20, y + 44), false))
          {
            if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)
              card_menu.focus = i;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !card_menu.fresh)
              choose = i;
          }
          if (i == card_menu.focus)
          {
            dl->AddRectFilled(cv.P(x0 + 20, y - 6), cv.P(x1 - 20, y + 44), IM_COL32(255, 255, 255, 18));
            theme::Chevron(dl, cv, x0 + 30, y + 6, 14);
          }
          theme::Text(dl, cv, fonts.menu, 36, x0 + 60, y, items[i].label,
                      items[i].enabled ? theme::color::kWhite : theme::color::kDim);
        }
        if (raw_in.accept && !card_menu.fresh)
          choose = card_menu.focus;
        card_menu.fresh = false;
        const int index = card_menu.index;
        if (choose < 0 && !card_menu.fresh && (raw_in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
          card_menu.open = false;
        else if (choose >= 0 && items[choose].enabled)
        {
          card_menu.open = false;
          switch (choose)
          {
          case 0: insert_card(index); break;
          case 1: open_naming(Naming::Mode::Rename, index); break;
          case 2: open_naming(Naming::Mode::Duplicate, index); break;
          case 3: ask_delete_card(index); break;
          default: break;
          }
        }
      }
    }

    // ---- yes / no ----
    if (confirm.open)
    {
      dl->AddRectFilled({0, 0}, viewport->Size, IM_COL32(0, 0, 0, 150));
      theme::SteelPanel(dl, cv, 360, 270, 920, 470, 16, IM_COL32(12, 20, 19, 245));
      float size = 36;
      if (theme::TextWidth(fonts.menu, size, confirm.title) > 500)
        size *= 500 / theme::TextWidth(fonts.menu, size, confirm.title);
      theme::Text(dl, cv, fonts.menu, size, 392, 296, confirm.title, theme::color::kGold);
      theme::Paragraph(dl, cv, fonts.body, 13, 394, 352, 496, confirm.detail, theme::color::kText, 22);
      const char* hint = gamepad_input ? "A DELETE    B KEEP" : "ENTER DELETE    ESC KEEP";
      theme::Text(dl, cv, fonts.small, 26, 888 - theme::TextWidth(fonts.small, 26, hint), 422, hint,
                  theme::color::kText);
      const bool answer_ok = !confirm.fresh;
      confirm.fresh = false;
      if (raw_in.accept && answer_ok)
      {
        confirm.open = false;
        if (confirm.yes)
          confirm.yes();
      }
      else if (answer_ok && (raw_in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        confirm.open = false;
    }

    // ---- naming dialog: a text box, and a keyboard for controllers ----
    if (naming.mode != Naming::Mode::None)
    {
      namespace osk = moderngekko::onscreen_keyboard;
      dl->AddRectFilled({0, 0}, viewport->Size, IM_COL32(0, 0, 0, 150));
      theme::SteelPanel(dl, cv, 380, 150, 900, 640, 16, IM_COL32(12, 20, 19, 245));
      const char* title = naming.mode == Naming::Mode::New    ? "NAME THE NEW CARD" :
                          naming.mode == Naming::Mode::Rename ? "RENAME THE CARD" :
                                                                "NAME THE COPY";
      theme::Text(dl, cv, fonts.menu, 36, 412, 174, title, theme::color::kGold);
      dl->AddRectFilled(cv.P(412, 230), cv.P(868, 274), IM_COL32(8, 12, 12, 230));
      dl->AddRect(cv.P(412, 230), cv.P(868, 274), IM_COL32(120, 126, 132, 255), 0, 0, cv.S(1));

      // Controller: D-pad / stick move, A types, X deletes, Start saves, B
      // cancels. Typing on it takes the text box out of keyboard focus, as
      // the box owns its text while focused.
      // Not on the frame it opened: the A that opened it is not a key press.
      const bool fresh = naming.focus;
      const auto pad = [fresh](ImGuiKey key) { return !fresh && ImGui::IsKeyPressed(key, true); };
      int dx = 0, dy = 0;
      if (pad(ImGuiKey_GamepadDpadLeft) || pad(ImGuiKey_GamepadLStickLeft))
        dx = -1;
      if (pad(ImGuiKey_GamepadDpadRight) || pad(ImGuiKey_GamepadLStickRight))
        dx = 1;
      if (pad(ImGuiKey_GamepadDpadUp) || pad(ImGuiKey_GamepadLStickUp))
        dy = -1;
      if (pad(ImGuiKey_GamepadDpadDown) || pad(ImGuiKey_GamepadLStickDown))
        dy = 1;
      if (dx || dy)
        osk::Move(&naming.cursor, dx, dy);
      bool commit = false;
      const auto edit = [&](const auto& change)
      {
        ImGui::ClearActiveID();
        std::string text = naming.text.data();
        if (!naming.typed)
        {
          text.clear();
          naming.typed = true;
        }
        change(text);
        std::snprintf(naming.text.data(), naming.text.size(), "%s", text.c_str());
      };
      if (!fresh && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false))
      {
        if (osk::KeyAt(naming.cursor) == osk::Key::Done)
          commit = true;
        else
          edit([&](std::string& text) { osk::Press(naming.cursor, &text, 24); });
      }
      if (pad(ImGuiKey_GamepadFaceLeft))
        edit([](std::string& text) {
          if (!text.empty())
            text.pop_back();
        });

      ImGui::SetCursorScreenPos(cv.P(426, 234));
      const ImGuiStyle& style = ImGui::GetStyle();
      ImGui::PushFont(fonts.menu ? fonts.menu : ImGui::GetFont(),
                      cv.S(36) / (style.FontScaleMain * style.FontScaleDpi));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Text, theme::color::kWhite);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
      ImGui::SetNextItemWidth(cv.S(430));
      if (naming.focus)
      {
        ImGui::SetKeyboardFocusHere();
        naming.focus = false;
      }
      const bool entered = ImGui::InputText(
          "##card_name", naming.text.data(), naming.text.size(),
          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter |
              ImGuiInputTextFlags_AutoSelectAll,
          CardNameFilter);
      if (ImGui::IsItemEdited())
        naming.typed = true;
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);
      ImGui::PopFont();
      if (!naming.error.empty())
        theme::Text(dl, cv, fonts.body, 13, 414, 284, naming.error, theme::color::kWarn, false);

      // The keyboard.
      const float key_w = 452.0f / osk::kColumns, key_h = 40, top = 306;
      for (int row = 0; row < osk::kRows; ++row)
      {
        const float y = top + row * (key_h + 6);
        const int keys = row < 4 ? osk::RowLength(row) : 3;
        for (int key = 0; key < keys; ++key)
        {
          const int c0 = row < 4 ? key : (key == 0 ? 0 : key == 1 ? 4 : 7);
          const int c1 = row < 4 ? key + 1 : (key == 0 ? 4 : key == 1 ? 7 : 10);
          const float x0 = 414 + c0 * key_w + 2, x1 = 414 + c1 * key_w - 2;
          const bool focus = naming.cursor.row == row &&
                             (row < 4 ? naming.cursor.column == key :
                                        osk::ActionAt(naming.cursor.column) == osk::ActionAt(c0));
          if (ImGui::IsMouseHoveringRect(cv.P(x0, y), cv.P(x1, y + key_h), false) &&
              ImGui::IsMouseClicked(ImGuiMouseButton_Left))
          {
            naming.cursor = {row, c0};
            if (osk::KeyAt(naming.cursor) == osk::Key::Done)
              commit = true;
            else
              edit([&](std::string& text) { osk::Press(naming.cursor, &text, 24); });
          }
          dl->AddRectFilled(cv.P(x0, y), cv.P(x1, y + key_h),
                            focus ? IM_COL32(110, 26, 30, 235) : IM_COL32(20, 30, 28, 230), cv.S(4));
          dl->AddRect(cv.P(x0, y), cv.P(x1, y + key_h),
                      focus ? theme::color::kRed : IM_COL32(120, 126, 132, 255), cv.S(4), 0,
                      cv.S(focus ? 2.0f : 1.0f));
          const std::string label =
              row < 4 ? std::string(1, osk::kKeys[row][key]) : osk::ActionLabel(osk::ActionAt(c0));
          const float size = row < 4 ? 26 : 22;
          if (label == "_")  // the game font has no underscore glyph
            dl->AddRectFilled(cv.P((x0 + x1) / 2 - 8, y + key_h - 13), cv.P((x0 + x1) / 2 + 8, y + key_h - 10),
                              theme::color::kWhite);
          else
            theme::Text(dl, cv, fonts.small, size,
                        (x0 + x1) / 2 - theme::TextWidth(fonts.small, size, label) / 2,
                        y + (key_h - size) / 2 - 1, label,
                        row == 4 && c0 == 7 ? theme::color::kGold : theme::color::kWhite);
        }
      }
      const char* hint = gamepad_input ? "A TYPE   X DELETE   START SAVE   B CANCEL" :
                                         "ENTER SAVE    ESC CANCEL";
      theme::Text(dl, cv, fonts.small, 22, 868 - theme::TextWidth(fonts.small, 22, hint), 600, hint,
                  theme::color::kText);
      if (entered || commit || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        commit_naming();
      else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
               ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        naming.mode = Naming::Mode::None;
    }
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 4, 9, 9, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    if (capture_directory)
    {
      if (capture_frame % kCaptureFrames == kCaptureFrames - 1)
      {
        if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr))
        {
          std::error_code ec;
          fs::create_directories(*capture_directory, ec);
          const fs::path file = *capture_directory / ("page" + std::to_string(capture_shot) + ".bmp");
          SDL_SaveBMP(shot, file.string().c_str());
          SDL_DestroySurface(shot);
        }
        if (capture_shot + 1 >= kCaptureShots)
          done = true;
      }
      ++capture_frame;
    }
    SDL_RenderPresent(renderer);
  }

  if (extraction_thread.joinable())
    extraction_thread.join();

  int result = 0;
  if (launch_mode != LaunchMode::None)
  {
    std::string launch_error;
    if (!WriteDefaultGame(user_directory, release_directory, current_game,
                          &launch_error))
    {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(), window);
      result = 1;
    }
    else
    {
      std::vector<std::string> argument_storage = {SiblingRunner(argv[0]).string(), "--game",
                                                   current_game.string(), "--user-dir",
                                                   user_directory.string(), "--automation-dir",
                                                   (user_directory / "automation").string()};
      // Solo only: see the note by the dropdown.
      if (launch_mode == LaunchMode::Solo && selected_savestate >= 0 &&
          static_cast<std::size_t>(selected_savestate) < launcher_savestates.size())
      {
        argument_storage.emplace_back("--load-state");
        argument_storage.emplace_back(
            launcher_savestates[static_cast<std::size_t>(selected_savestate)].string());
      }
      if (launch_mode == LaunchMode::BattleHost || launch_mode == LaunchMode::BattleJoin)
      {
        argument_storage.emplace_back("--env");
        argument_storage.emplace_back(
            launch_mode == LaunchMode::BattleHost ?
                "COLO_ONLINE_HOST=" + std::to_string(battle.port) :
                "COLO_ONLINE_JOIN=" + battle.address + ":" + std::to_string(battle.port));
        if (launch_mode == LaunchMode::BattleHost)
        {
          argument_storage.emplace_back("--env");
          argument_storage.emplace_back("COLO_ONLINE_PLAYER=" + std::to_string(battle.seat));
        }
      }
      if (launch_mode == LaunchMode::Host)
        argument_storage.emplace_back("--netplay-host");
      else if (launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-join");
        argument_storage.emplace_back(config.netplay_address);
      }
      if (launch_mode == LaunchMode::Host || launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-port");
        argument_storage.emplace_back(std::to_string(config.netplay_port));
        argument_storage.emplace_back("--nickname");
        argument_storage.emplace_back(config.netplay_nickname);
        argument_storage.emplace_back("--buffer");
        argument_storage.emplace_back(config.netplay_buffer);
        for (const std::string& controller : config.controllers)
        {
          argument_storage.emplace_back("--controller");
          argument_storage.emplace_back(controller);
        }
        if (!gba.rom.empty())
        {
          argument_storage.emplace_back("--gba-rom");
          argument_storage.emplace_back(gba.rom.string());
          if (!gba.bios.empty())
          {
            argument_storage.emplace_back("--gba-bios");
            argument_storage.emplace_back(gba.bios.string());
          }
          if (launch_mode == LaunchMode::Host && gba.player2)
          {
            argument_storage.emplace_back("--gba-port");
            argument_storage.emplace_back("2");
          }
        }
      }
      if (use_wayland)
        argument_storage.emplace_back("--wayland");
#if defined(__linux__)
      else
        argument_storage.emplace_back("-X11");
#endif
      std::vector<const char*> arguments;
      arguments.reserve(argument_storage.size() + 1);
      for (const std::string& argument : argument_storage)
        arguments.push_back(argument.c_str());
      arguments.push_back(nullptr);
      const std::filesystem::path log_path =
          user_directory / "Logs" / MODERNGEKKO_LOG_FILENAME;
      std::error_code log_error;
      std::filesystem::create_directories(log_path.parent_path(), log_error);
      SDL_IOStream* log_stream =
          log_error ? nullptr : SDL_IOFromFile(log_path.string().c_str(), "w");
      SDL_Process* process = nullptr;
      std::string process_error;
      if (log_stream)
      {
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties)
        {
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                 arguments.data());
          SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_REDIRECT);
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_stream);
          SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
                                 true);
          process = SDL_CreateProcessWithProperties(properties);
          SDL_DestroyProperties(properties);
        }
        if (!process)
          process_error = SDL_GetError();
        SDL_CloseIO(log_stream);
      }
      else
      {
        process = SDL_CreateProcess(arguments.data(), false);
        if (!process)
          process_error = SDL_GetError();
      }
      if (!process)
      {
        launch_error = "Could not start " + SiblingRunner(argv[0]).string() + ": " + process_error;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(),
                                 window);
        result = 1;
      }
      else
      {
        SDL_HideWindow(window);
        int exit_code = 1;
        const bool waited = SDL_WaitProcess(process, true, &exit_code);
        SDL_DestroyProcess(process);
        {
          std::string card_error;
          if (!card_library.CollectSlot(&card_error))
            SDL_Log("memory card library: %s", card_error.c_str());
        }
        if (!waited || exit_code != 0)
        {
          SDL_ShowWindow(window);
          if (!waited)
          {
            launch_error =
                "The game process could not be monitored: " + std::string(SDL_GetError());
          }
          else if (launch_mode == LaunchMode::Join)
          {
            switch (static_cast<moderngekko::frontend::NetplayExitCode>(exit_code))
            {
            case moderngekko::frontend::NetplayExitCode::VersionMismatch:
              launch_error = "The host is running an incompatible netplay build. Both "
                             "players must use the same release.";
              break;
            case moderngekko::frontend::NetplayExitCode::CompatibilityMismatch:
              launch_error = "The extracted game or recomp module does not match the "
                             "host. Both players need the same game revision and "
                             "release.";
              break;
            case moderngekko::frontend::NetplayExitCode::RoomFull:
              launch_error = "All four controller slots are already occupied.";
              break;
            case moderngekko::frontend::NetplayExitCode::GameRunning:
              launch_error = "The host has already started the game.";
              break;
            case moderngekko::frontend::NetplayExitCode::ServerFull:
              launch_error = "The netplay server is full.";
              break;
            case moderngekko::frontend::NetplayExitCode::NicknameRejected:
              launch_error = "The nickname was rejected by the host.";
              break;
            default:
              launch_error = "Could not reach the netplay host. Check the host name, UDP "
                             "port, and firewall.";
              break;
            }
          }
          else if (launch_mode == LaunchMode::Host)
          {
            launch_error = "Could not create the netplay session. Check the UDP port "
                           "and firewall settings.";
          }
          else
          {
            launch_error = "The game process exited with code " + std::to_string(exit_code) + ".";
          }
          if (!log_error)
            launch_error += "\n\nDetails were written to:\n" + log_path.string();
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Session failed", launch_error.c_str(),
                                   window);
          result = 1;
        }
      }
    }
  }

  for (const auto& [path, texture] : thumbnails)
    if (texture)
      SDL_DestroyTexture(texture);
  if (backdrop)
    SDL_DestroyTexture(backdrop);
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
