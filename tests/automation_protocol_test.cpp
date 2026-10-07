#include "automation_protocol.hpp"

#include <filesystem>
#include <fstream>

namespace
{
std::filesystem::path MakeTempDirectory()
{
  const auto path = std::filesystem::temp_directory_path() /
                    std::filesystem::path("moderngekko_automation_test");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}
}

int main()
{
  namespace automation = moderngekko::automation;

  if (automation::ResolveColosseumAuthoredCanvasSideWidth(5120, 1440) != 1600 ||
      automation::ResolveColosseumAuthoredCanvasSideWidth(5119, 1439) != 1600 ||
      automation::ResolveColosseumAuthoredCanvasSideWidth(1920, 1080) != 240 ||
      automation::ResolveColosseumAuthoredCanvasSideWidth(1280, 960) != 0 ||
      automation::ResolveColosseumAuthoredCanvasSideWidth(0, 1440) != 0)
  {
    return 18;
  }

  if (automation::ResolveColosseumNamingBackdropSideWidth(5120, 1440) != 1657 ||
      automation::ResolveColosseumNamingBackdropSideWidth(5119, 1439) != 1657 ||
      automation::ResolveColosseumNamingBackdropSideWidth(1920, 1080) != 282 ||
      automation::ResolveColosseumNamingBackdropSideWidth(1280, 960) != 38 ||
      automation::ResolveColosseumNamingBackdropSideWidth(0, 1440) != 0)
  {
    return 19;
  }

  // The naming background's bottom quad restarts the four-row stripe pattern
  // at authored row 367. The ultrawide fill must reproduce that one-row phase
  // advance at both exact 3x height and the 1439-line borderless presentation.
  if (automation::IsColosseumNamingBackdropLightBand(0, 1439) ||
      automation::IsColosseumNamingBackdropLightBand(11, 1439) ||
      !automation::IsColosseumNamingBackdropLightBand(12, 1439) ||
      !automation::IsColosseumNamingBackdropLightBand(1100, 1439) ||
      automation::IsColosseumNamingBackdropLightBand(1101, 1439) ||
      automation::IsColosseumNamingBackdropLightBand(1112, 1439) ||
      !automation::IsColosseumNamingBackdropLightBand(1113, 1439) ||
      !automation::IsColosseumNamingBackdropLightBand(1436, 1439) ||
      automation::IsColosseumNamingBackdropLightBand(1437, 1439) ||
      !automation::IsColosseumNamingBackdropLightBand(1100, 1440) ||
      automation::IsColosseumNamingBackdropLightBand(1101, 1440) ||
      automation::IsColosseumNamingBackdropLightBand(-1, 1440) ||
      automation::IsColosseumNamingBackdropLightBand(0, 0))
  {
    return 20;
  }

  if (!automation::IsColosseumFieldMenuMessage(0x013a) ||
      !automation::IsColosseumFieldMenuMessage(0x013d) ||
      automation::IsColosseumFieldMenuMessage(0x0139) ||
      automation::IsColosseumFieldMenuMessage(0x013e) ||
      automation::IsColosseumFieldMenuMessage(11070) ||
      !automation::IsColosseumFieldMenuState(true, false, true, 0x18, 1, 0x013a) ||
      !automation::IsColosseumFieldMenuState(false, true, true, 0x18, 1, 0x013a) ||
      automation::IsColosseumFieldMenuState(false, false, true, 0x18, 1, 0x013a) ||
      automation::IsColosseumFieldMenuState(true, false, true, 0x18, 0, 0x013a) ||
      !automation::IsColosseumPdaMessage(0x3600) ||
      !automation::IsColosseumPdaMessage(0x36ff) ||
      automation::IsColosseumPdaMessage(0x3500) ||
      automation::IsColosseumPdaMessage(0x3700) ||
      !automation::IsColosseumCharacterNamingMessage(0x0069) ||
      !automation::IsColosseumCharacterNamingMessage(0x2ef5) ||
      !automation::IsColosseumCharacterNamingMessage(0x2efd) ||
      automation::IsColosseumCharacterNamingMessage(0x0068) ||
      automation::IsColosseumCharacterNamingMessage(0x006a) ||
      automation::IsColosseumCharacterNamingMessage(0x2ef4) ||
      automation::IsColosseumCharacterNamingMessage(0x2ef6) ||
      automation::IsColosseumCharacterNamingMessage(0x2efc) ||
      automation::IsColosseumCharacterNamingMessage(0x2efe) ||
      !automation::IsColosseumCharacterNamingState(0x2efd, false) ||
      !automation::IsColosseumCharacterNamingState(0x2efd, true) ||
      // The keyboard entry screen. Observed as message 0x2ef5 with the
      // menu byte raised; it must match regardless of that byte, exactly
      // like name selection.
      !automation::IsColosseumCharacterNamingState(0x2ef5, false) ||
      !automation::IsColosseumCharacterNamingState(0x2ef5, true) ||
      automation::IsColosseumCharacterNamingState(0x0069, false) ||
      !automation::IsColosseumCharacterNamingState(0x0069, true) ||
      // Every observed message in the standalone front-end is the same kind of
      // authored 4:3 canvas, and the screen changes message per highlighted
      // option, so the whole band must resolve the same way. Classifying only
      // the difficulty popup left the mode select, the Battle Now root and the
      // confirmation on the Hor+ projection, and the canvas snapped between
      // aspects on every button press.
      !automation::IsColosseumBattleNowMenu(0x3c14) ||
      !automation::IsColosseumBattleNowMenu(0x3c15) ||
      !automation::IsColosseumBattleNowMenu(0x3c2d) ||
      !automation::IsColosseumBattleNowMenu(0x3c2f) ||
      !automation::IsColosseumBattleNowMenu(0x3d49) ||
      !automation::IsColosseumBattleNowMenu(0x3d6c) ||
      !automation::IsColosseumBattleNowMenu(0x3d8b) ||
      // A single stray arena message during a menu transition must not confirm
      // battle gameplay; a real battle keeps reporting one.
      automation::IsColosseumBattleGameplayConfirmed(0) ||
      automation::IsColosseumBattleGameplayConfirmed(1) ||
      automation::IsColosseumBattleGameplayConfirmed(2) ||
      !automation::IsColosseumBattleGameplayConfirmed(3) ||
      !automation::IsColosseumBattleGameplayConfirmed(60) ||
      // Band edges.
      !automation::IsColosseumBattleNowMenu(0x3c00) ||
      !automation::IsColosseumBattleNowMenu(0x3dff) ||
      automation::IsColosseumBattleNowMenu(0x3bff) ||
      automation::IsColosseumBattleNowMenu(0x3e00) ||
      // The live arena reports from separate bands and must stay ultrawide.
      automation::IsColosseumBattleNowMenu(0x00ce) ||
      automation::IsColosseumBattleNowMenu(0x00cf) ||
      automation::IsColosseumBattleNowMenu(0x00e9) ||
      automation::IsColosseumBattleNowMenu(0x013e) ||
      automation::IsColosseumBattleNowMenu(0x30d4) ||
      automation::IsColosseumBattleNowMenu(0x30db) ||
      automation::IsColosseumBattleNowMenu(0x30e6) ||
      !automation::IsColosseumBattleGameplayMessage(0x013e) ||
      !automation::IsColosseumBattleGameplayMessage(0x00cf) ||
      !automation::IsColosseumBattleGameplayMessage(0x00d0) ||
      !automation::IsColosseumBattleGameplayMessage(0x00e7) ||
      automation::IsColosseumBattleGameplayMessage(0x00ce) ||
      automation::IsColosseumBattleGameplayMessage(0x00e6) ||
      automation::IsColosseumBattleGameplayMessage(0x013d) ||
      automation::IsColosseumBattleGameplayMessage(0x013f) ||
      !automation::IsColosseumMoviePlaybackActive(1, 1) ||
      automation::IsColosseumMoviePlaybackActive(0, 1) ||
      automation::IsColosseumMoviePlaybackActive(1, 0) ||
      !automation::IsColosseumLiveTitleSceneActive(false, false, false) ||
      automation::IsColosseumLiveTitleSceneActive(true, false, false) ||
      automation::IsColosseumLiveTitleSceneActive(false, true, false) ||
      automation::IsColosseumLiveTitleSceneActive(false, false, true) ||
      automation::ResolveColosseumNavigationOverlayMode(0, false) != 0 ||
      automation::ResolveColosseumNavigationOverlayMode(0, true) != 1 ||
      automation::ResolveColosseumNavigationOverlayMode(1, true) != 1 ||
      automation::ResolveColosseumNavigationOverlayMode(2, true) != 2 ||
      !automation::ShouldStartColosseumUltrawideGameplay(true, false, false) ||
      !automation::ShouldStartColosseumUltrawideGameplay(false, true, false) ||
      !automation::ShouldStartColosseumUltrawideGameplay(false, false, true) ||
      automation::ShouldStartColosseumUltrawideGameplay(false, false, false) ||
      !automation::ShouldUseColosseumAuthoredMenuPresentation(true, true, false) ||
      automation::ShouldUseColosseumAuthoredMenuPresentation(true, false, false) ||
      automation::ShouldUseColosseumAuthoredMenuPresentation(false, true, false) ||
      automation::ShouldUseColosseumAuthoredMenuPresentation(false, true, true) ||
      !automation::ShouldUseColosseumAuthoredMenuPresentation(false, false, true) ||
      !automation::ShouldUseColosseumAuthoredMenuPresentation(true, false, false, true) ||
      automation::ShouldUseColosseumAuthoredMenuPresentation(false, false, false, true) ||
      // Live battle gameplay overrides an otherwise-authored menu frame. The
      // Battle Now band no longer appears here at all: those front-end screens
      // present Hor+ like the OPTIONS screen beside them, so there is no
      // battle_now parameter left to pin them to the authored canvas.
      automation::ShouldUseColosseumAuthoredMenuPresentation(
          true, true, false, true, true) ||
      !automation::ShouldUseColosseumAuthoredMenuPresentation(
          false, false, false, true, false, true) ||
      !automation::ShouldUseColosseumAuthoredMenuPresentation(
          false, true, false, true, false, true) ||
      automation::ShouldUseColosseumAuthoredMenuPresentation(
          true, true, false, true, true, true) ||
      !automation::ResolveColosseumBattleGameplaySession(
          false, false, true, false, false, false) ||
      !automation::ResolveColosseumBattleGameplaySession(
          true, false, false, false, false, false) ||
      automation::ResolveColosseumBattleGameplaySession(
          true, true, false, false, false, false) ||
      automation::ResolveColosseumBattleGameplaySession(
          true, false, false, true, false, false) ||
      automation::ResolveColosseumBattleGameplaySession(
          true, false, false, false, true, false) ||
      automation::ResolveColosseumBattleGameplaySession(
          true, false, false, false, false, true) ||
      // A STORY battle keeps the field actor table alive, so navigation stays
      // valid while fighting. Without the battle-field signal the session never
      // opens and the command menu is read as a complete 4:3 canvas, which
      // pillarboxes a live battle on an ultrawide display.
      !automation::ResolveColosseumBattleGameplaySession(
          false, false, true, true, false, false, true) ||
      // The same frame without the battle field is the walking case the
      // original rule was protecting: navigation valid means not fighting.
      automation::ResolveColosseumBattleGameplaySession(
          false, false, true, true, false, false, false) ||
      // Hysteresis still carries a story battle across a message id that
      // leaves the arena band, as it already did for Battle Now.
      !automation::ResolveColosseumBattleGameplaySession(
          true, false, false, true, false, false, true) ||
      // A battle field does not override the hard disqualifiers.
      automation::ResolveColosseumBattleGameplaySession(
          true, true, false, true, false, false, true) ||
      automation::ResolveColosseumBattleGameplaySession(
          true, false, false, true, true, false, true) ||
      automation::ResolveColosseumBattleGameplaySession(
          true, false, false, true, false, true, true) ||
      // The archive predicate itself: _bf marks a battle field, and the
      // walkable area it is paired with must not match.
      // The story-mode battle command menu, and the phases around it that must
      // stay outside the band. 0x0067 and 0x019f were sampled on live battles
      // that already presented correctly, so admitting them would change frames
      // that are not broken.
      !automation::IsColosseumBattleGameplayMessage(0x0140u) ||
      !automation::IsColosseumBattleGameplayMessage(0x013eu) ||
      automation::IsColosseumBattleGameplayMessage(0x0067u) ||
      automation::IsColosseumBattleGameplayMessage(0x019fu) ||
      !automation::IsColosseumBattleFieldArchive("M1_out_bf") ||
      !automation::IsColosseumBattleFieldArchive("M1_water_colo") ||
      !automation::ShouldRetainColosseumBattleNowArena(0x97, 0x97) ||
      !automation::ShouldRetainColosseumBattleNowArena(0x97, 0) ||
      !automation::ShouldRetainColosseumBattleNowArena(0x97, 0xa1, false) ||
      automation::ShouldRetainColosseumBattleNowArena(0x97, 0xa1) ||
      automation::ShouldRetainColosseumBattleNowArena(0, 0) ||
      automation::IsColosseumBattleNowArenaArchive("M1_out_bf") ||
      !automation::IsColosseumBattleFieldArchive("M2_earth_colo") ||
      !automation::IsColosseumBattleFieldArchive("M4_bottom_colo") ||
      !automation::IsColosseumBattleFieldArchive("M4_cylinder_colo") ||
      !automation::IsColosseumBattleFieldArchive("T1_ancient_colo") ||
      automation::IsColosseumBattleFieldArchive("D2_crater_colo") ||
      automation::IsColosseumBattleFieldArchive("D4_casino_colo") ||
      !automation::ResolveColosseumBattleGameplaySession(
          false, false, false, true, false, false, true) ||
      automation::IsColosseumBattleFieldArchive("M1_out") ||
      automation::IsColosseumBattleFieldArchive("S1_out") ||
      automation::IsColosseumBattleFieldArchive("") ||
      automation::IsColosseumBattleFieldArchive("_bf") ||
      !automation::ShouldUseColosseumUltrawideProjection(true, true, false) ||
      automation::ShouldUseColosseumUltrawideProjection(true, true, true) ||
      automation::ShouldUseColosseumUltrawideProjection(true, false, false) ||
      automation::ShouldUseColosseumUltrawideProjection(false, true, false) ||
      !automation::ShouldUseColosseumUltrawideProjection(true, false, false, true, false) ||
      automation::ShouldUseColosseumUltrawideProjection(true, false, false, true, true) ||
      !automation::ShouldApplyColosseumProjectionPresentation(true, true, false) ||
      !automation::ShouldApplyColosseumProjectionPresentation(true, false, true) ||
      !automation::ShouldApplyColosseumProjectionPresentation(true, false, false, true, false) ||
      !automation::ShouldApplyColosseumProjectionPresentation(true, false, false, false, true) ||
      automation::ShouldApplyColosseumProjectionPresentation(true, false, false) ||
      automation::ShouldApplyColosseumProjectionPresentation(false, true, true) ||
      automation::ResolveColosseumAuthoredBackdrop(true, false, true) !=
          automation::ColosseumAuthoredBackdrop::Naming ||
      automation::ResolveColosseumAuthoredBackdrop(true, false, false) !=
          automation::ColosseumAuthoredBackdrop::None ||
      automation::ResolveColosseumAuthoredBackdrop(false, false, true) !=
          automation::ColosseumAuthoredBackdrop::None ||
      automation::ResolveColosseumAuthoredBackdrop(true, true, true) !=
          automation::ColosseumAuthoredBackdrop::None ||
      !automation::ShouldOffsetColosseumPyriteSky(true, true, false, 0x0f,
                                                  "M2_out") ||
      automation::ShouldOffsetColosseumPyriteSky(false, true, false, 0x0f,
                                                  "M2_out") ||
      automation::ShouldOffsetColosseumPyriteSky(true, true, true, 0x0f,
                                                  "M2_out") ||
      automation::ShouldOffsetColosseumPyriteSky(true, true, false, 0x0e,
                                                  "M2_out") ||
      automation::ShouldOffsetColosseumPyriteSky(true, true, false, 0x0f,
                                                  "S1_out"))
  {
    return 17;
  }

  const std::filesystem::path root = MakeTempDirectory();
  const std::filesystem::path commands = root / "commands";
  std::filesystem::create_directories(commands);

  {
    std::ofstream output(commands / "002_pad.txt");
    output << "# comment\n"
           << "command=pad\n"
           << "port=1\n"
           << "a=1\n"
           << "main_x=0.5\n"
           << "main_y=-1\n";
  }
  {
    std::ofstream output(commands / "001_pause.txt");
    output << "command=pause\n";
  }

  const auto listed = automation::ListCommandFiles(commands);
  if (listed.size() != 2 || listed[0].filename() != "001_pause.txt" ||
      listed[1].filename() != "002_pad.txt")
  {
    return 1;
  }

  automation::Command command;
  std::string error;
  if (!automation::ParseCommandFile(commands / "002_pad.txt", &command, &error))
    return 2;
  if (command.type != automation::CommandType::Pad || command.pad.port != 1)
    return 3;
  if (command.pad.controls[0] != 1.0 || command.pad.controls[14] != 0.5 ||
      command.pad.controls[15] != -1.0)
  {
    return 4;
  }

  {
    std::ofstream output(commands / "003_bad.txt");
    output << "command=pad\nport=0\nunknown=1\n";
  }
  error.clear();
  if (automation::ParseCommandFile(commands / "003_bad.txt", &command, &error) ||
      error.find("unknown pad field") == std::string::npos)
  {
    return 5;
  }

  automation::Status status;
  status.state = "running";
  status.booted = true;
  status.fps = 60.0;
  status.frame_count = 120;
  status.present_count = 121;
  status.navigation_mode = 2;
  status.navigation_valid = true;
  status.navigation_room_id = 3;
  status.navigation_player_x = -13.0f;
  status.navigation_player_z = 168.0f;
  status.navigation_collision_valid = true;
  status.navigation_collision_base = 0x809f58c0;
  status.navigation_collision_segments = 184;
  status.navigation_npc_count = 2;
  status.navigation_archive = "M1_stadium_1F";
  status.navigation_location = "Phenac Stadium 1F";
  status.text_upscale_enabled = true;
  status.text_upscale_count = 42;
  status.last_command = "002_pad.txt";
  const std::string formatted = automation::FormatStatus(status);
  if (!formatted.contains("state=running\n") ||
      !formatted.contains("booted=1\n") ||
      !formatted.contains("frame_count=120\n") ||
      !formatted.contains("present_count=121\n") ||
      !formatted.contains("navigation_mode=2\n") ||
      !formatted.contains("navigation_valid=1\n") ||
      !formatted.contains("movie_active=0\n") ||
      !formatted.contains("live_title_scene_active=0\n") ||
      !formatted.contains("authored_menu_active=0\n") ||
      !formatted.contains("menu_edge_fill_active=0\n") ||
      !formatted.contains("navigation_room_id=3\n") ||
      !formatted.contains("navigation_player_x=-13\n") ||
      !formatted.contains("navigation_collision_valid=1\n") ||
      !formatted.contains("navigation_collision_base=2157926592\n") ||
      !formatted.contains("navigation_collision_segments=184\n") ||
      !formatted.contains("navigation_npc_count=2\n") ||
      !formatted.contains("navigation_archive=M1_stadium_1F\n") ||
      !formatted.contains("navigation_location=Phenac Stadium 1F\n") ||
      !formatted.contains("text_upscale_enabled=1\n") ||
      !formatted.contains("text_upscale_count=42\n") ||
      !formatted.contains("last_command=002_pad.txt\n"))
  {
    return 6;
  }

  {
    std::ofstream output(commands / "004_read_memory.txt");
    output << "command=read_memory\n"
           << "address=0x809e52b0\n"
           << "size=32\n"
           << "path=artifacts/player.bin\n";
  }
  error.clear();
  if (!automation::ParseCommandFile(commands / "004_read_memory.txt", &command, &error))
    return 7;
  if (command.type != automation::CommandType::ReadMemory ||
      command.address != 0x809e52b0 || command.size != 32 ||
      command.path != std::filesystem::path("artifacts/player.bin"))
  {
    return 8;
  }

  {
    std::ofstream output(commands / "005_bad_memory.txt");
    output << "command=read_memory\n"
           << "address=0xfffffff0\n"
           << "size=32\n"
           << "path=artifacts/bad.bin\n";
  }
  error.clear();
  if (automation::ParseCommandFile(commands / "005_bad_memory.txt", &command, &error) ||
      error.find("exceed the guest address space") == std::string::npos)
  {
    return 9;
  }

  {
    std::ofstream output(commands / "006_write_memory.txt");
    output << "command=write_memory\n"
           << "address=0x809e52bc\n"
           << "data=42 f6 00 00\n";
  }
  error.clear();
  if (!automation::ParseCommandFile(commands / "006_write_memory.txt", &command, &error))
    return 10;
  if (command.type != automation::CommandType::WriteMemory ||
      command.address != 0x809e52bc || command.size != 4 ||
      command.data != std::vector<std::uint8_t>({0x42, 0xf6, 0x00, 0x00}))
  {
    return 11;
  }

  {
    std::ofstream output(commands / "007_pad_frames.txt");
    output << "command=pad_frames\n"
           << "port=0\n"
           << "frames=3\n"
           << "main_y=1\n";
  }
  error.clear();
  if (!automation::ParseCommandFile(commands / "007_pad_frames.txt", &command, &error))
    return 12;
  if (command.type != automation::CommandType::PadFrames || command.frames != 3 ||
      command.pad.port != 0 || command.pad.controls[15] != 1.0)
  {
    return 13;
  }

  {
    std::ofstream output(commands / "008_bad_pad_frames.txt");
    output << "command=pad_frames\nport=0\nframes=0\na=1\n";
  }
  error.clear();
  if (automation::ParseCommandFile(commands / "008_bad_pad_frames.txt", &command, &error) ||
      error.find("frames=1..36000") == std::string::npos)
  {
    return 14;
  }

  // The front-end message window is what keeps a stale Battle Now message id
  // from latching the authored 4:3 canvas onto the attract title after Quit.
  if (!automation::IsColosseumMessageWindowLive(2) ||
      automation::IsColosseumMessageWindowLive(0) ||
      automation::IsColosseumMessageWindowLive(1))
  {
    return 40;
  }

  // A live window arms the hold; losing it counts down and never goes negative.
  if (automation::ResolveColosseumFrontEndDismissHold(true, 0) !=
          automation::kColosseumFrontEndDismissFields ||
      automation::ResolveColosseumFrontEndDismissHold(true, 5) !=
          automation::kColosseumFrontEndDismissFields ||
      automation::ResolveColosseumFrontEndDismissHold(false, 5) != 4 ||
      automation::ResolveColosseumFrontEndDismissHold(false, 0) != 0)
  {
    return 41;
  }

  // Resident while live, resident while the hold survives, gone once it drains.
  if (!automation::IsColosseumFrontEndWindowResident(true, 0) ||
      !automation::IsColosseumFrontEndWindowResident(false, 1) ||
      automation::IsColosseumFrontEndWindowResident(false, 0))
  {
    return 42;
  }

  // A screen-to-screen fade sits at state 1 for just under three seconds, so
  // the hold must outlast it; quitting to the title never rebuilds the window,
  // so the hold must still be finite.
  {
    int hold = automation::ResolveColosseumFrontEndDismissHold(true, 0);
    int fields = 0;
    while (automation::IsColosseumFrontEndWindowResident(false, hold))
    {
      hold = automation::ResolveColosseumFrontEndDismissHold(false, hold);
      ++fields;
    }
    if (fields != automation::kColosseumFrontEndDismissFields || fields <= 180)
      return 43;
  }




  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return 0;
}
