#pragma once

#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moderngekko::automation
{
constexpr int ResolveColosseumAuthoredCanvasSideWidth(int width, int height)
{
  if (width <= 0 || height <= 0)
    return 0;

  // Round height * 4/3 to the nearest whole presentation pixel. Naming uses
  // the full authored 640x480 canvas; the narrower 602x480 PDA content width
  // must not be reused here because it overpaints the model at both edges.
  const int authored_width = (height * 4 + 1) / 3;
  return width > authored_width ? (width - authored_width) / 2 : 0;
}

constexpr int ResolveColosseumNamingBackdropSideWidth(int width, int height)
{
  if (width <= 0 || height <= 0)
    return 0;

  // Naming UI is centered into the 602-pixel VI safe aperture so its outer
  // frames remain visible. Extend the generated background through the same
  // inset; all foreground geometry is deliberately inside this boundary.
  constexpr int naming_content_width = 602;
  constexpr int naming_content_height = 480;
  const int content_width =
      (height * naming_content_width + naming_content_height / 2) /
      naming_content_height;
  return width > content_width ? (width - content_width) / 2 : 0;
}

constexpr bool IsColosseumNamingBackdropLightBand(int output_y, int output_height)
{
  if (output_y < 0 || output_y >= output_height || output_height <= 0)
    return false;

  constexpr int authored_height = 480;
  constexpr int band_height = 4;
  constexpr int bottom_section_start = 367;
  const int authored_y = static_cast<int>(
      static_cast<std::int64_t>(output_y) * authored_height / output_height);

  // The bottom background quad begins one source row before the uninterrupted
  // stripe sequence would change. Mirror that authored seam so the generated
  // ultrawide margins remain phase-aligned all the way to the bottom edge.
  const int phased_y = authored_y >= bottom_section_start ? authored_y + 1 : authored_y;
  return ((phased_y / band_height) & 1) != 0;
}

constexpr bool IsColosseumFieldMenuMessage(std::uint32_t message_id)
{
  return message_id >= 0x013a && message_id <= 0x013d;
}

constexpr bool IsColosseumFieldMenuState(bool menu_active, bool field_overlay_active,
                                         bool navigation_valid,
                                         std::uint32_t field_overlay_mode,
                                         std::uint32_t field_overlay_phase,
                                         std::uint32_t message_id)
{
  // The message id and phase remain resident after the field menu closes; one
  // of the live menu signals must still be raised before they can identify it.
  return (menu_active || field_overlay_active) && navigation_valid &&
         field_overlay_mode == 0x18 && field_overlay_phase != 0 &&
         IsColosseumFieldMenuMessage(message_id);
}

constexpr bool IsColosseumPdaMessage(std::uint32_t message_id)
{
  return (message_id & 0xff00) == 0x3600;
}

constexpr bool IsColosseumCharacterNamingMessage(std::uint32_t message_id)
{
  // 0x0069 is the live confirmation prompt ("Is <name> OK?") captured by
  // active_ui_message_id. Keep the older 0x2efd selection-screen identifier
  // so both phases retain their authored 4:3 canvas.
  // 0x2ef5 is the keyboard entry screen. It was missing here, so the entry
  // phase lost the authored correction that 0x2efd (name selection) receives:
  // the naming model rendered condensed and the authored backdrop margins were
  // replaced by black pillarbox.
  return message_id == 0x0069u || message_id == 0x2ef5u ||
         message_id == 0x2efdu;
}

constexpr bool IsColosseumCharacterNamingState(std::uint32_t message_id,
                                                bool menu_active)
{
  // 0x0069 is reused by the title screen's "start a new Story?" prompt,
  // where the menu-active byte is clear. The character-name confirmation
  // raises that byte, while 0x2efd uniquely identifies name selection and
  // 0x2ef5 the keyboard entry screen. Both of the latter are unambiguous, so
  // neither needs the menu-active qualifier.
  return message_id == 0x2ef5u || message_id == 0x2efdu ||
         (message_id == 0x0069u && menu_active);
}

constexpr bool IsColosseumBattleNowMenu(std::uint32_t message_id)
{
  // The standalone Battle Now flow never raises the field-menu flag or
  // populates the field actor table, so its UI message is the only thing that
  // identifies the authored canvas.
  //
  // This whole front-end is authored as complete 4:3 canvases, and it assigns
  // a distinct message per screen AND per highlighted option, so enumerating
  // them individually kept missing cases and the canvas snapped between 4:3
  // and squeezed-into-a-column on every button press. Observed so far:
  //   0x3c14  mode select, STORY MODE highlighted
  //   0x3c15  mode select, BATTLE MODE highlighted
  //   0x3c2d  Battle Now root, SINGLE BATTLE highlighted
  //   0x3c2f  Battle Now root, DOUBLE BATTLE highlighted
  //   0x3d49  "enter with these POKeMON?" confirmation
  //   0x3d6c  difficulty popup
  //   0x3d8b  fade between the confirmation and the arena
  //
  // Every message seen in 0x3c00..0x3dff is one of these authored screens, and
  // the live arena reports from entirely separate bands (0x00ce/0x00cf/0x00e9/
  // 0x013e and 0x30d4/0x30db/0x30e6), so match the band rather than the list.
  return message_id >= 0x3c00u && message_id <= 0x3dffu;
}

// The front-end never clears active_ui_message_id when it hands the screen back
// to the attract title. Quitting out of mode select leaves the last highlighted
// entry's id (0x3c1d) resident for the rest of the session, so a band match on
// its own latches the authored 4:3 canvas onto the live title scene forever:
// the title comes back pillarboxed and never recovers.
//
// lbl_80402418 is the message window object that owns that id (the id lives at
// its +0x1E). Its first byte is the window state: 0 before any window has been
// built, 2 while one is live, 1 once it has been dismissed. Dumping the whole
// 0x68 struct on a pristine boot, in the menu, and after Quit, this byte is the
// only field that separates "menu is up" from "menu is gone" -- the id, the
// text pointers and the rest are left exactly as the front-end wrote them.
constexpr std::uint32_t kColosseumMessageWindowLive = 2;

constexpr bool IsColosseumMessageWindowLive(std::uint32_t window_state)
{
  return window_state == kColosseumMessageWindowLive;
}

// The window also sits at state 1 while one front-end screen fades into the
// next -- measured at just under three seconds for Colosseum Battle -> mode
// select -- so the state cannot be used raw without the authored canvas
// snapping to Hor+ mid-fade. Hold it across any gap shorter than this, and
// release once the window has stayed dismissed for longer than the longest
// transition observed. Quitting to the title never rebuilds the window, so
// this hold is what finally ends the latch there.
//
// Known limit: highlighting the mode-select Quit entry also drops the window
// to state 1 and leaves the id at 0x3c1d, which is byte-for-byte what the
// screen looks like after Quit has actually been taken. Dumping a megabyte of
// bss in both states and filtering for words that are stable in one and not
// the other turned up no field that separates them -- the id, the window
// struct, the scene callback stack, and the attract counter all match. So
// resting on Quit for longer than this hold presents that one menu entry at
// Hor+ until the cursor moves. That is the deliberate trade for the title
// screen no longer being pillarboxed permanently; raising the constant trades
// it back the other way.
constexpr int kColosseumFrontEndDismissFields = 240;

constexpr bool IsColosseumFrontEndWindowResident(bool window_live, int dismiss_hold_fields)
{
  return window_live || dismiss_hold_fields > 0;
}

constexpr int ResolveColosseumFrontEndDismissHold(bool window_live, int dismiss_hold_fields)
{
  if (window_live)
    return kColosseumFrontEndDismissFields;
  return dismiss_hold_fields > 0 ? dismiss_hold_fields - 1 : 0;
}

constexpr bool IsColosseumBattleGameplayMessage(std::uint32_t message_id)
{
  // Battle Now keeps the generic menu flag raised throughout combat, so that
  // flag cannot distinguish the authored setup canvas from the live arena.
  // 0x00cf starts the trainer introduction, 0x00d0 covers targeting/attack,
  // 0x00e7 is move selection, and 0x013e is the battle command menu. Recognize
  // each as an entry point so savestates can resume directly into any phase.
  // 0x0140 is the story-mode battle command menu, the analogue of Battle Now's
  // 0x013e. Story battles raise the generic complete-menu flag on this screen,
  // so without it the live battle is read as an authored 4:3 canvas and gets
  // pillarboxed on an ultrawide display. Observed on two independent battles -
  // Phenac (M1_out_bf) and Outskirt Stand (S1_out_bf) - while every other
  // sampled battle phase reports 0x0067, 0x00d0 or 0x019f and is unaffected.
  return message_id == 0x00cfu || message_id == 0x00d0u ||
         message_id == 0x00e7u || message_id == 0x013eu ||
         message_id == 0x0140u;
}

// Colosseum publishes the arena's UI message a field or two before the
// authored Battle Now canvas finishes tearing down, so a single field can read
// as battle gameplay while the menu is still on screen. Confirming the arena
// after one field let that stray read end the authored presentation, and the
// screen flashed Hor+ for the length of the presentation debounce. Require the
// observation to persist; a real battle keeps reporting it, a transition does
// not.
constexpr int kColosseumBattleGameplayConfirmFields = 3;

// The same stray field also takes the message id out of the authored band, so
// the Battle Now classification needs a matching hold. Keep it shorter than a
// confirmed arena so a real battle is never delayed: the confirm above clears
// this hold outright.
constexpr int kColosseumBattleNowHoldFields = 6;

constexpr bool IsColosseumBattleGameplayConfirmed(int consecutive_message_fields)
{
  return consecutive_message_fields >= kColosseumBattleGameplayConfirmFields;
}

// Whether the loaded collision archive is a battle field rather than a walkable
// area. Story encounters use _bf. Standalone Battle Now also uses the five
// dedicated colosseum scenes below, whose navigation table remains valid.
// Do not match every _colo name: the crater/casino walking maps have _bf arenas.
constexpr bool IsColosseumBattleNowArenaArchive(std::string_view archive)
{
  return archive == "M1_water_colo" || archive == "M2_earth_colo" ||
         archive == "M4_bottom_colo" || archive == "M4_cylinder_colo" ||
         archive == "T1_ancient_colo";
}

constexpr bool IsColosseumBattleFieldArchive(std::string_view archive)
{
  return (archive.size() > 3 && archive.substr(archive.size() - 3) == "_bf") ||
         IsColosseumBattleNowArenaArchive(archive);
}

constexpr bool ShouldRetainColosseumBattleNowArena(unsigned int arena_room,
                                                  unsigned int current_room,
                                                  bool navigation_valid = true)
{
  // Room zero and a missing actor table are transient during arena loading;
  // neither is a confirmed transition back to a walking map.
  return arena_room != 0 &&
         (!navigation_valid || current_room == 0 || arena_room == current_room);
}

constexpr bool ResolveColosseumBattleGameplaySession(
    bool session_active, bool battle_now_menu_active, bool battle_gameplay_message,
    bool navigation_valid, bool character_naming_active, bool movie_active,
    bool battle_field_archive = false)
{
  // Battle UI message ids change during move selection, targeting, and attack
  // animation. Once combat is identified, retain Hor+ until a stable scene
  // boundary proves that the battle has ended.
  if (battle_now_menu_active || character_naming_active || movie_active)
    return false;
  // navigation_valid used to disqualify a battle outright, which was right for
  // the standalone Battle Now front-end this was written against: it loads no
  // field, so valid navigation meant the player was walking around. A STORY
  // battle keeps the field actor table alive, so navigation stays valid while
  // fighting, the session never opened, and the command menu was then read as a
  // complete 4:3 canvas - pillarboxing a live battle on an ultrawide display.
  // Let the battle field itself settle it: valid navigation only rules a battle
  // out when the loaded area is somewhere you can walk.
  if (navigation_valid && !battle_field_archive)
    return false;
  // Standalone arenas are already battle scenes during their loading camera,
  // before a trainer/command message is published. Their menus must not reset
  // presentation while the navigation table remains alive.
  return session_active || battle_gameplay_message || battle_field_archive;
}

enum class ColosseumAuthoredBackdrop : std::uint8_t
{
  None,
  Naming,
};

constexpr ColosseumAuthoredBackdrop ResolveColosseumAuthoredBackdrop(
    bool widescreen_enabled, bool movie_active, bool character_naming_active)
{
  if (!widescreen_enabled || movie_active)
    return ColosseumAuthoredBackdrop::None;
  if (character_naming_active)
    return ColosseumAuthoredBackdrop::Naming;
  return ColosseumAuthoredBackdrop::None;
}

constexpr int ResolveColosseumNavigationOverlayMode(int configured_mode,
                                                     bool minimap_enabled)
{
  // The minimap feature controls the compact map's presentation, but mode 0
  // prevents the map from being drawn at all. Preserve explicit compact/debug
  // choices and promote Off only when the feature is enabled.
  return minimap_enabled && configured_mode == 0 ? 1 : configured_mode;
}

constexpr bool ShouldStartColosseumUltrawideGameplay(bool booted_from_savestate,
                                                      bool navigation_valid,
                                                      bool menu_active)
{
  return booted_from_savestate || navigation_valid || menu_active;
}

constexpr bool ShouldUseColosseumAuthoredMenuPresentation(bool complete_menu_active,
                                                           bool navigation_valid,
                                                           bool character_naming_active,
                                                           bool gameplay_started = false,
                                                           bool battle_gameplay_active = false,
                                                           bool battle_now_menu_active = false)
{
  // Field submenus and the pre-field character naming scene are authored as
  // complete 4:3 canvases. Applying the Hor+ projection multiplier to the
  // naming model makes it unnaturally thin, so preserve that canvas even
  // before the field actor table exists.
  // The field actor table is absent on Battle Now/Colosseum submenu canvases and
  // immediately after loading a savestate. Once gameplay has started, a complete
  // menu bit is sufficient evidence that the frame is an authored 4:3 canvas.
  //
  // Battle Now's popup backing uses authored clipping while its border/text
  // use the menu projection. Keep the whole setup canvas at its authored aspect
  // so both agree. An identified arena takes precedence over menu overlays.
  return !battle_gameplay_active &&
         (battle_now_menu_active ||
          (!navigation_valid && character_naming_active) ||
          (complete_menu_active && (navigation_valid || gameplay_started)));
}

constexpr bool ShouldUseColosseumUltrawideProjection(bool widescreen_enabled,
                                                       bool gameplay_started,
                                                       bool complete_menu_active,
                                                       bool live_title_scene_active = false,
                                                       bool movie_active = false)
{
  return widescreen_enabled && !complete_menu_active && !movie_active &&
         (gameplay_started || live_title_scene_active);
}

constexpr bool ShouldApplyColosseumProjectionPresentation(bool widescreen_enabled,
                                                            bool gameplay_started,
                                                            bool authored_menu_active,
                                                            bool live_title_scene_active = false,
                                                            bool movie_active = false)
{
  // Character naming is an authored 4:3 scene before field gameplay starts.
  // Its embedded model still needs the authored-menu projection correction.
  return widescreen_enabled &&
         (gameplay_started || authored_menu_active || live_title_scene_active || movie_active);
}

constexpr bool ShouldOffsetColosseumPyriteSky(bool widescreen_enabled,
                                               bool gameplay_started,
                                               bool complete_menu_active,
                                               std::uint32_t room_id,
                                               std::string_view archive_name)
{
  return ShouldUseColosseumUltrawideProjection(
             widescreen_enabled, gameplay_started, complete_menu_active) &&
         room_id == 0x0f && archive_name == "M2_out";
}


enum class CommandType
{
  Pad,
  PadFrames,
  ClearPad,
  Pause,
  Resume,
  SaveState,
  LoadState,
  Screenshot,
  ReadMemory,
  WriteMemory,
  Stop,
};

struct PadState
{
  int port = 0;
  std::array<double, ciface::Touch::LAST_GC_CONTROL - ciface::Touch::FIRST_GC_CONTROL + 1>
      controls{};
};

struct Command
{
  CommandType type = CommandType::Pause;
  std::string source_name;
  PadState pad;
  std::filesystem::path path;
  std::filesystem::path screenshot_path;
  std::uint32_t address = 0;
  std::uint32_t size = 0;
  std::uint32_t frames = 0;
  std::vector<std::uint8_t> data;
};

struct Status
{
  std::string state = "stopped";
  bool booted = false;
  double fps = 0.0;
  double vps = 0.0;
  double speed = 0.0;
  std::uint64_t frame_count = 0;
  std::uint64_t present_count = 0;
  std::string title;
  std::string game_id;
  std::string game_name;
  bool text_upscale_enabled = false;
  std::uint64_t text_upscale_count = 0;
  int navigation_mode = 0;
  bool navigation_valid = false;
  bool movie_active = false;
  bool live_title_scene_active = false;
  bool authored_menu_active = false;
  bool menu_edge_fill_active = false;
  // Presentation telemetry. aspect_mode is the resolved Dolphin AspectMode
  // (-1 before the first decision); presentation_transitions is monotonic so a
  // sampler polling slower than the field rate still sees every change.
  bool naming_presentation_active = false;
  bool gameplay_ultrawide = false;
  // The two guest words IsColosseumCharacterNamingState reads, surfaced so a
  // mismatch can be diagnosed without attaching a debugger.
  unsigned int colosseum_message_id = 0;
  bool colosseum_menu_active = false;
  int aspect_mode = -1;
  std::uint64_t presentation_transitions = 0;
  unsigned int navigation_room_id = 0;
  float navigation_player_x = 0.0f;
  float navigation_player_z = 0.0f;
  float navigation_facing = 0.0f;
  bool navigation_collision_valid = false;
  std::uint32_t navigation_collision_base = 0;
  std::uint32_t navigation_collision_segments = 0;
  std::uint32_t navigation_npc_count = 0;
  std::string navigation_archive;
  std::string navigation_location;
  std::uint64_t processed_commands = 0;
  std::string last_command;
  std::string last_error;
};

inline constexpr bool IsColosseumMoviePlaybackActive(std::uint8_t player_ready,
                                                      std::uint8_t movie_open)
{
  // This is the exact predicate used by THPPlayerGetState (fn_801E1874).
  return player_ready != 0 && movie_open != 0;
}

inline constexpr bool IsColosseumLiveTitleSceneActive(bool movie_active,
                                                       bool navigation_valid,
                                                       bool authored_scene_active)
{
  // Before the field actor table exists, every non-movie, non-authored frame
  // is part of the live title fly-through. The New Game confirmation is an
  // overlay on that live scene and must keep the animated background active.
  // Logos and the opening cinematic use THP; name entry has its own gate.
  return !movie_active && !navigation_valid && !authored_scene_active;
}

std::filesystem::path ResolveControlPath(const std::filesystem::path& automation_directory,
                                         const std::filesystem::path& value);
std::vector<std::filesystem::path>
ListCommandFiles(const std::filesystem::path& commands_directory);
bool ParseCommandFile(const std::filesystem::path& path, Command* command, std::string* error);
std::string FormatStatus(const Status& status);
}  // namespace moderngekko::automation
