#pragma once

// A library of named memory cards for a one-save-per-card game.
//
// Each card is one .gci file in <User>/MemoryCards ("Save 1.gci", "test1.gci");
// a zero-length file is a blank card. The card "in Slot A" is the one Dolphin
// sees: its save sits in the GCI-folder Slot A directory (<User>/GC/<region>/
// Card A) under Dolphin's own file name. The library keeps one rule: the slot
// always holds the inserted card, so before anything replaces the slot its
// contents are copied back to that card (keeping the previous copy as
// <name>.gci.bak). Saves made outside the launcher are therefore never lost.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace moderngekko::frontend
{
struct LibraryCard
{
  std::string name;
  std::filesystem::path file;
  bool blank = true;
  int blocks = 0;
  std::optional<std::filesystem::file_time_type> saved;
  // From <card>.gci.info, written by the runtime when the game saves.
  std::string location;
  int playtime = -1;  // seconds
};

class MemoryCardLibrary
{
public:
  // game_id is the six-character disc ID (GC6E01): the first four characters
  // pick the game's saves out of the slot, the fourth its region folder.
  MemoryCardLibrary(std::filesystem::path user_directory, std::string game_id);

  // Lists the library, creating it from whatever is in Slot A the first time,
  // and collects the slot into the inserted card.
  bool Load(std::string* error);

  const std::vector<LibraryCard>& Cards() const { return m_cards; }
  int Inserted() const { return m_inserted; }

  // Puts card `index` into Slot A (collecting the current card first).
  bool Insert(int index, std::string* error);
  bool Create(const std::string& name, std::string* error);
  bool Duplicate(int index, const std::string& name, std::string* error);
  bool Rename(int index, const std::string& name, std::string* error);
  // Removes a card (and its .bak) with `remove`, e.g. a move to the Recycle
  // Bin. The card in Slot A cannot be deleted; insert another first.
  bool Delete(int index, const std::function<bool(const std::filesystem::path&)>& remove,
              std::string* error);

  // Copies Slot A back into the inserted card: after a play session.
  bool CollectSlot(std::string* error);

  // Letters, digits, spaces, '-' and '_', 1 to 24 characters, not taken.
  bool NameAvailable(const std::string& name) const;
  static bool ValidName(const std::string& name);
  std::string SuggestName() const;  // "Save N"

private:
  std::vector<std::filesystem::path> SlotSaves() const;
  bool List(std::string* error);
  bool WriteInserted(std::string* error);

  std::filesystem::path m_library;
  std::filesystem::path m_slot;
  std::string m_game_code;
  std::vector<LibraryCard> m_cards;
  int m_inserted = -1;
};
}  // namespace moderngekko::frontend
