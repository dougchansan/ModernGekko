#include "players_ui.hpp"

#include "gba_screen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/NetPlay/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

void Host_RequestAudioMuteToggle();
bool Host_IsAudioMuted();

namespace moderngekko::players_ui
{
namespace
{
using SerialInterface::SIDevices;

enum class Socket
{
  Empty,
  Controller,
  Gba,
};

struct State
{
  std::mutex mutex;
  std::filesystem::path config_path;  // config.ini
  std::filesystem::path gba_rom;      // used for GBAs plugged in from the menu
  std::filesystem::path gba_bios;
  std::array<int, 4> gba_volume{50, 50, 50, 50};  // percent
  std::array<bool, 4> gba_muted{};
  bool volumes_dirty = true;
};
State& s_state = *new State();

std::atomic<bool> s_players_open{false};
std::atomic<bool> s_mixer_open{false};
std::atomic<bool> s_wants_mouse{false};

constexpr std::array<const char*, 3> kSocketNames = {"Nothing", "GameCube Controller",
                                                     "Game Boy Advance"};

const char* SocketKey(Socket socket)
{
  switch (socket)
  {
  case Socket::Empty:
    return "none";
  case Socket::Gba:
    return "gba";
  default:
    return "controller";
  }
}

Socket SocketOf(SIDevices device)
{
  switch (device)
  {
  case SerialInterface::SIDEVICE_NONE:
    return Socket::Empty;
  case SerialInterface::SIDEVICE_GC_GBA_EMULATED:
    return Socket::Gba;
  default:
    return Socket::Controller;
  }
}

std::string FileName(const std::filesystem::path& path)
{
  return path.filename().string();
}

// Writes some keys to config.ini, keeping everything else in it.
template <typename Fn>
void SaveIni(Fn&& fill)
{
  std::filesystem::path path;
  {
    std::lock_guard lock(s_state.mutex);
    path = s_state.config_path;
  }
  if (path.empty())
    return;
  Common::IniFile ini;
  ini.Load(path.string());
  fill(ini);
  ini.Save(path.string());
}

void SaveScreens()
{
  const gba_screen::Settings settings = gba_screen::GetSettings();
  SaveIni([&](Common::IniFile& ini) {
    auto* players = ini.GetOrCreateSection("Players");
    players->Set("gba_numbers", settings.show_numbers);
    for (int i = 0; i < 4; ++i)
    {
      const std::string n = std::to_string(i + 1);
      const gba_screen::View& view = settings.views[i];
      players->Set("gba_opacity" + n, view.opacity);
      players->Set("gba_window" + n, view.detached);
      players->Set("gba_x" + n, view.x);
      players->Set("gba_y" + n, view.y);
      players->Set("gba_width" + n, view.width);
    }
  });
}

void SaveVolumes()
{
  std::array<int, 4> volume;
  std::array<bool, 4> muted;
  {
    std::lock_guard lock(s_state.mutex);
    volume = s_state.gba_volume;
    muted = s_state.gba_muted;
  }
  SaveIni([&](Common::IniFile& ini) {
    auto* audio = ini.GetOrCreateSection("Audio");
    audio->Set("volume", Config::Get(Config::MAIN_AUDIO_VOLUME));
    for (int i = 0; i < 4; ++i)
    {
      audio->Set("gba_volume" + std::to_string(i + 1), volume[i]);
      audio->Set("gba_mute" + std::to_string(i + 1), muted[i]);
    }
  });
}

void SaveSocket(int port, Socket socket)
{
  SaveIni([&](Common::IniFile& ini) {
    ini.GetOrCreateSection("Players")->Set("port" + std::to_string(port + 1),
                                          std::string(SocketKey(socket)));
  });
}

// A GBA is plugged in with the ROM it will run (and the BIOS that boots it).
bool PrepareGba(int port)
{
#ifdef HAS_LIBMGBA
  std::filesystem::path rom, bios;
  {
    std::lock_guard lock(s_state.mutex);
    rom = s_state.gba_rom;
    bios = s_state.gba_bios;
  }
  if (rom.empty())
    return false;
  Config::SetCurrent(Config::MAIN_GBA_ROM_PATHS[port], rom.string());
  if (!bios.empty())
  {
    Config::SetCurrent(Config::MAIN_GBA_BIOS_PATH, bios.string());
    File::SetUserPath(F_GBABIOS_IDX, bios.string());
  }
  return true;
#else
  static_cast<void>(port);
  return false;
#endif
}

SIDevices DeviceFor(Socket socket)
{
  switch (socket)
  {
  case Socket::Empty:
    return SerialInterface::SIDEVICE_NONE;
  case Socket::Gba:
    return SerialInterface::SIDEVICE_GC_GBA_EMULATED;
  default:
    return SerialInterface::SIDEVICE_GC_CONTROLLER;
  }
}

void Plug(int port, Socket socket)
{
  if (socket == Socket::Gba && !PrepareGba(port))
    return;
  const SIDevices device = DeviceFor(socket);
  // For this run; config.ini keeps the choice for the next.
  Config::SetCurrent(Config::GetInfoForSIDevice(port), device);
  // The panel draws on the video thread, and pausing the CPU from there waits
  // on the video thread itself: hand the change to the window thread.
  Core::QueueHostJob([port, device](Core::System& system) {
    if (Core::IsRunning(system))
      Core::RunOnCPUThread(system, [&system, port, device] {
        system.GetSerialInterface().ChangeDevice(device, port);
      });
  });
  SaveSocket(port, socket);
}

bool IsGamepad(const std::string& device)
{
  return device.starts_with("SDL/") || device.starts_with("XInput/");
}

// A device's buttons in the GameCube controller's places: positional pad
// names, which Dolphin matches on any gamepad (as the launcher writes them).
void MapController(ControllerEmu::EmulatedController* pad, const std::string& device)
{
  Common::IniFile ini;
  auto* map = ini.GetOrCreateSection("pad");
  map->Set("Device", device);
  map->Set("Buttons/A", std::string("`Button A`"));
  map->Set("Buttons/B", std::string("`Button B`"));
  map->Set("Buttons/X", std::string("`Button X`"));
  map->Set("Buttons/Y", std::string("`Button Y`"));
  map->Set("Buttons/Z", std::string("`Shoulder R`"));
  map->Set("Buttons/Start", std::string("Start"));
  map->Set("Main Stick/Up", std::string("`Left Y+`"));
  map->Set("Main Stick/Down", std::string("`Left Y-`"));
  map->Set("Main Stick/Left", std::string("`Left X-`"));
  map->Set("Main Stick/Right", std::string("`Left X+`"));
  map->Set("Main Stick/Calibration", std::string("100.00"));
  map->Set("C-Stick/Up", std::string("`Right Y+`"));
  map->Set("C-Stick/Down", std::string("`Right Y-`"));
  map->Set("C-Stick/Left", std::string("`Right X-`"));
  map->Set("C-Stick/Right", std::string("`Right X+`"));
  map->Set("C-Stick/Calibration", std::string("100.00"));
  map->Set("Triggers/L", std::string("`Trigger L`"));
  map->Set("Triggers/R", std::string("`Trigger R`"));
  map->Set("Triggers/L-Analog", std::string("`Trigger L`"));
  map->Set("Triggers/R-Analog", std::string("`Trigger R`"));
  map->Set("D-Pad/Up", std::string("`Pad N`"));
  map->Set("D-Pad/Down", std::string("`Pad S`"));
  map->Set("D-Pad/Left", std::string("`Pad W`"));
  map->Set("D-Pad/Right", std::string("`Pad E`"));
  map->Set("Rumble/Motor", std::string("`Motor L` | `Motor R`"));
  pad->LoadConfig(map);
}

// The GBA's buttons where a handheld has them: A on the right face button,
// B below it, L and R on the shoulders, Select and Start in the middle.
void MapGba(ControllerEmu::EmulatedController* pad, const std::string& device)
{
  Common::IniFile ini;
  auto* map = ini.GetOrCreateSection("gba");
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
  pad->LoadConfig(map);
}

InputConfig* ConfigFor(Socket socket)
{
  if (socket == Socket::Gba)
    return Pad::IsGBAInitialized() ? Pad::GetGBAConfig() : nullptr;
  return Pad::IsInitialized() ? Pad::GetConfig() : nullptr;
}

std::string DeviceOf(Socket socket, int port)
{
  InputConfig* config = ConfigFor(socket);
  if (!config || port >= config->GetControllerCount())
    return {};
  auto lock = ControllerEmu::EmulatedController::GetStateLock();
  return config->GetController(port)->GetDefaultDevice().ToString();
}

void UseDevice(Socket socket, int port, const std::string& device)
{
  InputConfig* config = ConfigFor(socket);
  if (!config || port >= config->GetControllerCount())
    return;
  {
    auto lock = ControllerEmu::EmulatedController::GetStateLock();
    ControllerEmu::EmulatedController* pad = config->GetController(port);
    if (IsGamepad(device))
    {
      if (socket == Socket::Gba)
        MapGba(pad, device);
      else
        MapController(pad, device);
    }
    else
    {
      // A keyboard (or anything else) gets Dolphin's own layout for it.
      pad->LoadDefaults(g_controller_interface);
      pad->SetDefaultDevice(device);
    }
    pad->UpdateReferences(g_controller_interface);
  }
  config->SaveConfig();
}

void ApplyVolumes()
{
  auto& system = Core::System::GetInstance();
  SoundStream* stream = system.GetSoundStream();
  if (!stream || !stream->GetMixer())
    return;
  std::array<int, 4> volume;
  std::array<bool, 4> muted;
  {
    std::lock_guard lock(s_state.mutex);
    if (!s_state.volumes_dirty)
    {
      static SoundStream* s_last_stream = nullptr;
      if (s_last_stream == stream)
        return;
      s_last_stream = stream;
    }
    s_state.volumes_dirty = false;
    volume = s_state.gba_volume;
    muted = s_state.gba_muted;
  }
  for (int i = 0; i < 4; ++i)
  {
    const u32 level = muted[i] ? 0u : static_cast<u32>(volume[i] * 256 / 100);
    stream->GetMixer()->SetGBAVolume(static_cast<std::size_t>(i), level, level);
  }
}

// Panels open centred near the top, at a set width; their height fits what
// they hold (an auto-sized window does not know its width on its first frame,
// so centring it by its own size put it half off the screen).
void PlacePanel(float width)
{
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  width = std::min(width, display.x - 24.0f);
  ImGui::SetNextWindowPos(ImVec2(std::round((display.x - width) * 0.5f), 48.0f),
                          ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Appearing);
}

void DrawPlayers()
{
  bool open = s_players_open.load();
  PlacePanel(720.0f);
  if (!ImGui::Begin("Players", &open,
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
  {
    ImGui::End();
    s_players_open = open;
    return;
  }
  const bool netplay = NetPlay::IsNetPlayRunning();
  std::filesystem::path rom;
  {
    std::lock_guard lock(s_state.mutex);
    rom = s_state.gba_rom;
  }
  if (netplay)
    ImGui::TextWrapped("Netplay decides the sockets while a session is running.");

  std::vector<std::string> devices = g_controller_interface.GetAllDeviceStrings();
  ImGui::BeginDisabled(netplay);
  if (ImGui::BeginTable("sockets", 3))
  {
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn("Plugged in", ImGuiTableColumnFlags_WidthFixed, 200);
    ImGui::TableSetupColumn("Played with", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int port = 0; port < 4; ++port)
    {
      ImGui::PushID(port);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Player %d", port + 1);

      const Socket socket = SocketOf(Config::Get(Config::GetInfoForSIDevice(port)));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##socket", kSocketNames[static_cast<int>(socket)]))
      {
        for (int s = 0; s < 3; ++s)
        {
          const bool needs_rom = s == static_cast<int>(Socket::Gba) && rom.empty();
          ImGui::BeginDisabled(needs_rom);
          if (ImGui::Selectable(kSocketNames[s], s == static_cast<int>(socket)))
            Plug(port, static_cast<Socket>(s));
          ImGui::EndDisabled();
          if (needs_rom)
            ImGui::SetItemTooltip("Start once with --gba-rom to choose the GBA game");
        }
        ImGui::EndCombo();
      }

      ImGui::TableNextColumn();
      if (socket == Socket::Empty)
      {
        ImGui::TextDisabled("-");
      }
      else
      {
        const std::string current = DeviceOf(socket, port);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##device", current.empty() ? "None" : current.c_str()))
        {
          for (const std::string& device : devices)
            if (ImGui::Selectable(device.c_str(), device == current))
              Core::QueueHostJob([socket, port, device](Core::System&) {
                UseDevice(socket, port, device);
              });
          ImGui::EndCombo();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  if (!rom.empty())
    ImGui::TextDisabled("GBA game: %s", FileName(rom).c_str());
  ImGui::TextDisabled("Each player's GBA keeps its own save (...-2.sav for player 2, and so on).");
  ImGui::TextDisabled("Right-click a GBA screen for its opacity, its own window or a reset.");
  ImGui::TextDisabled("Hold L+R+Start+Select on a GBA for a second to turn it off and on.");
  gba_screen::Settings screens = gba_screen::GetSettings();
  if (ImGui::Checkbox("Show player numbers on GBA screens", &screens.show_numbers))
  {
    gba_screen::SetSettings(screens);
    SaveScreens();
  }
  ImGui::End();
  s_players_open = open;
}

void DrawMixer()
{
  bool open = s_mixer_open.load();
  PlacePanel(600.0f);
  if (!ImGui::Begin("Audio Mixer", &open,
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
  {
    ImGui::End();
    s_mixer_open = open;
    return;
  }
  bool save = false;
  int master = Config::Get(Config::MAIN_AUDIO_VOLUME);
  ImGui::SetNextItemWidth(260);
  if (ImGui::SliderInt("Everything", &master, 0, 100, "%d%%"))
  {
    Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, master);
    Core::QueueHostJob([](Core::System& system) { AudioCommon::UpdateSoundStream(system); });
  }
  save |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SameLine();
  bool muted_all = Host_IsAudioMuted();
  if (ImGui::Checkbox("Mute##all", &muted_all))
    Host_RequestAudioMuteToggle();

  ImGui::SeparatorText("Game Boy Advances");
  for (int i = 0; i < 4; ++i)
  {
    ImGui::PushID(i);
    int volume;
    bool muted;
    {
      std::lock_guard lock(s_state.mutex);
      volume = s_state.gba_volume[i];
      muted = s_state.gba_muted[i];
    }
    const bool running = gba_screen::IsActive(i);
    char label[32];
    std::snprintf(label, sizeof(label), "Player %d GBA%s", i + 1, running ? "" : " (off)");
    ImGui::SetNextItemWidth(260);
    const bool moved = ImGui::SliderInt(label, &volume, 0, 100, "%d%%");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    const bool toggled = ImGui::Checkbox("Mute", &muted);
    if (moved || toggled)
    {
      std::lock_guard lock(s_state.mutex);
      s_state.gba_volume[i] = volume;
      s_state.gba_muted[i] = muted;
      s_state.volumes_dirty = true;
    }
    save |= toggled;
    ImGui::PopID();
  }
  ImGui::TextDisabled("The GBAs play through the same speakers as the game.");
  ImGui::End();
  s_mixer_open = open;
  if (save)
    SaveVolumes();
}
}  // namespace

void Initialize(const RuntimeConfig& config)
{
  Common::IniFile ini;
  const std::filesystem::path path = config.user_directory / "config.ini";
  ini.Load(path.string());
  auto* players = ini.GetOrCreateSection("Players");
  auto* audio = ini.GetOrCreateSection("Audio");
  auto* gba = ini.GetOrCreateSection("GBA");

  // The GBA game: from the command line (remembered for next time), or the
  // one remembered.
  std::filesystem::path rom, bios = config.input.gba_bios;
  for (const auto& path_for_port : config.input.gba_roms)
    if (!path_for_port.empty())
      rom = path_for_port;
  std::string saved;
  if (rom.empty() && gba->Get("rom", &saved) && !saved.empty())
    rom = saved;
  if (bios.empty() && gba->Get("bios", &saved) && !saved.empty())
    bios = saved;

  {
    std::lock_guard lock(s_state.mutex);
    s_state.config_path = path;
    s_state.gba_rom = rom;
    s_state.gba_bios = bios;
    for (int i = 0; i < 4; ++i)
    {
      audio->Get("gba_volume" + std::to_string(i + 1), &s_state.gba_volume[i], 50);
      audio->Get("gba_mute" + std::to_string(i + 1), &s_state.gba_muted[i], false);
      s_state.gba_volume[i] = std::clamp(s_state.gba_volume[i], 0, 100);
    }
    s_state.volumes_dirty = true;
  }
  int volume = 100;
  if (audio->Get("volume", &volume, 100))
    Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, std::clamp(volume, 0, 100));

  gba_screen::Settings screens;
  players->Get("gba_numbers", &screens.show_numbers, true);
  for (int i = 0; i < 4; ++i)
  {
    const std::string n = std::to_string(i + 1);
    gba_screen::View& view = screens.views[i];
    players->Get("gba_opacity" + n, &view.opacity, 80);
    players->Get("gba_window" + n, &view.detached, false);
    players->Get("gba_x" + n, &view.x, -1.0f);
    players->Get("gba_y" + n, &view.y, -1.0f);
    players->Get("gba_width" + n, &view.width, 0.0f);
  }
  gba_screen::SetSettings(screens);
  gba_screen::SetChangedCallback(SaveScreens);

  // Remember the GBA game the command line named.
  if (!config.input.gba_bios.empty() || !rom.empty())
  {
    if (!rom.empty())
      gba->Set("rom", rom.string());
    if (!bios.empty())
      gba->Set("bios", bios.string());
    ini.Save(path.string());
  }

  // Plug in the sockets saved from the Players panel, unless netplay or the
  // command line already decided them.
  if (NetPlay::IsNetPlayRunning())
    return;
  for (int port = 0; port < 4; ++port)
  {
    if (!config.input.gba_roms[port].empty())
      continue;
    std::string key;
    if (!players->Get("port" + std::to_string(port + 1), &key))
      continue;
    const Socket socket = key == "gba"  ? Socket::Gba :
                          key == "none" ? Socket::Empty :
                                          Socket::Controller;
    if (socket == Socket::Gba && !PrepareGba(port))
      continue;
    Config::SetCurrent(Config::GetInfoForSIDevice(port), DeviceFor(socket));
  }
}

void Draw()
{
  ApplyVolumes();
  gba_screen::Draw();
  if (s_players_open)
    DrawPlayers();
  if (s_mixer_open)
    DrawMixer();
  s_wants_mouse = ImGui::GetIO().WantCaptureMouse || s_players_open || s_mixer_open;
}

void Tick()
{
  gba_screen::Tick();
}
}  // namespace moderngekko::players_ui

void Host_OpenPlayersPanel()
{
  moderngekko::players_ui::s_players_open = true;
}

void Host_OpenAudioMixer()
{
  moderngekko::players_ui::s_mixer_open = true;
}

bool Host_IsGbaPlayerNumbersShown()
{
  return moderngekko::gba_screen::GetSettings().show_numbers;
}

void Host_ToggleGbaPlayerNumbers()
{
  auto settings = moderngekko::gba_screen::GetSettings();
  settings.show_numbers = !settings.show_numbers;
  moderngekko::gba_screen::SetSettings(settings);
  moderngekko::players_ui::SaveScreens();
}

bool Host_WantsMouse()
{
  return moderngekko::players_ui::s_wants_mouse;
}
