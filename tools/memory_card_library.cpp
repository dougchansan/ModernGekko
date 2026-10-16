#include "memory_card_library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace moderngekko::frontend
{
namespace fs = std::filesystem;
namespace
{
constexpr const char* kInsertedFile = "slot-a.txt";

std::vector<char> ReadAll(const fs::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool WriteAll(const fs::path& path, const std::vector<char>& data, std::string* error)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (file)
    return true;
  if (error)
    *error = "could not write " + path.string();
  return false;
}

// A card's date is when the game last saved it, not when the library copied
// it: copies carry the source file's modification time across.
void KeepTime(const fs::path& from, const fs::path& to)
{
  std::error_code ec;
  const auto time = fs::last_write_time(from, ec);
  if (!ec)
    fs::last_write_time(to, time, ec);
}

// Beside a card or a slot save: what the runtime recorded when the game last
// saved it (see UpdateMemoryCardInfo).
fs::path InfoFor(const fs::path& save)
{
  return fs::path(save).concat(".info");
}

// Copies (or, with no source, removes) the .info beside `to`.
void CarryInfo(const fs::path& from, const fs::path& to)
{
  std::error_code ec;
  if (!from.empty() && fs::exists(InfoFor(from), ec))
    fs::copy_file(InfoFor(from), InfoFor(to), fs::copy_options::overwrite_existing, ec);
  else
    fs::remove(InfoFor(to), ec);
}

std::string Lower(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// Dolphin's GCI-folder name for a save: <maker>-<game code>-<file name>.gci,
// all taken from the save's own directory entry.
std::string DolphinFileName(const std::vector<char>& gci)
{
  if (gci.size() < 0x40)
    return "save.gci";
  std::string name(gci.data() + 8, gci.data() + 8 + 32);
  name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
  return std::string(gci.data() + 4, 2) + "-" + std::string(gci.data(), 4) + "-" + name + ".gci";
}

const char* RegionFolder(char region)
{
  switch (region)
  {
  case 'J': return "JAP";
  case 'P': case 'D': case 'F': case 'S': case 'I': case 'U': return "EUR";
  default: return "USA";
  }
}
}  // namespace

MemoryCardLibrary::MemoryCardLibrary(fs::path user_directory, std::string game_id)
    : m_library(user_directory / "MemoryCards"),
      m_slot(user_directory / "GC" / RegionFolder(game_id.size() > 3 ? game_id[3] : 'E') /
             "Card A"),
      m_game_code(game_id.substr(0, 4))
{
}

bool MemoryCardLibrary::ValidName(const std::string& name)
{
  if (name.empty() || name.size() > 24 || name.front() == ' ' || name.back() == ' ')
    return false;
  return std::ranges::all_of(name, [](unsigned char c)
                             { return std::isalnum(c) || c == ' ' || c == '-' || c == '_'; });
}

bool MemoryCardLibrary::NameAvailable(const std::string& name) const
{
  return ValidName(name) &&
         std::ranges::none_of(m_cards, [&](const LibraryCard& card)
                              { return Lower(card.name) == Lower(name); });
}

std::string MemoryCardLibrary::SuggestName() const
{
  for (int n = 1;; ++n)
    if (NameAvailable("Save " + std::to_string(n)))
      return "Save " + std::to_string(n);
}

std::vector<fs::path> MemoryCardLibrary::SlotSaves() const
{
  std::vector<fs::path> saves;
  std::error_code ec;
  for (fs::directory_iterator it(m_slot, ec), end; !ec && it != end; it.increment(ec))
  {
    if (!it->is_regular_file(ec) || it->path().extension() != ".gci")
      continue;
    char code[4] = {};
    std::ifstream(it->path(), std::ios::binary).read(code, 4);
    if (std::string(code, 4) == m_game_code)
      saves.push_back(it->path());
  }
  return saves;
}

bool MemoryCardLibrary::List(std::string* error)
{
  std::string inserted_name;
  if (m_inserted >= 0)
    inserted_name = m_cards[static_cast<std::size_t>(m_inserted)].name;
  else
    std::getline(std::ifstream(m_library / kInsertedFile), inserted_name);
  while (!inserted_name.empty() && std::isspace(static_cast<unsigned char>(inserted_name.back())))
    inserted_name.pop_back();

  m_cards.clear();
  std::error_code ec;
  for (fs::directory_iterator it(m_library, ec), end; !ec && it != end; it.increment(ec))
  {
    if (!it->is_regular_file(ec) || it->path().extension() != ".gci")
      continue;
    LibraryCard card;
    card.name = it->path().stem().string();
    card.file = it->path();
    const auto size = it->file_size(ec);
    card.blank = ec || size < 0x40;
    if (!card.blank)
    {
      const std::vector<char> header = [&] {
        std::vector<char> h(0x40);
        std::ifstream(card.file, std::ios::binary).read(h.data(), 0x40);
        return h;
      }();
      card.blocks = (static_cast<unsigned char>(header[0x38]) << 8) |
                    static_cast<unsigned char>(header[0x39]);
      std::error_code time_ec;
      const auto time = it->last_write_time(time_ec);
      if (!time_ec)
        card.saved = time;
    }
    {
      std::ifstream info(InfoFor(card.file));
      std::string line;
      while (std::getline(info, line))
      {
        if (line.starts_with("location="))
          card.location = line.substr(9);
        else if (line.starts_with("playtime="))
          card.playtime = std::atoi(line.c_str() + 9);
      }
    }
    m_cards.push_back(std::move(card));
  }
  std::ranges::sort(m_cards, [](const LibraryCard& a, const LibraryCard& b)
                    { return Lower(a.name) < Lower(b.name); });
  m_inserted = -1;
  for (std::size_t i = 0; i < m_cards.size(); ++i)
    if (Lower(m_cards[i].name) == Lower(inserted_name))
      m_inserted = static_cast<int>(i);
  if (ec && error)
    *error = "can't list " + m_library.string() + ": " + ec.message();
  return !ec;
}

bool MemoryCardLibrary::WriteInserted(std::string* error)
{
  std::ofstream file(m_library / kInsertedFile, std::ios::trunc);
  if (m_inserted >= 0)
    file << m_cards[static_cast<std::size_t>(m_inserted)].name << '\n';
  if (file)
    return true;
  if (error)
    *error = "could not write " + (m_library / kInsertedFile).string();
  return false;
}

bool MemoryCardLibrary::Load(std::string* error)
{
  std::error_code ec;
  fs::create_directories(m_library, ec);
  if (ec)
  {
    if (error)
      *error = "can't create " + m_library.string() + ": " + ec.message();
    return false;
  }
  m_inserted = -1;
  if (!List(error))
    return false;
  if (m_inserted < 0)
  {
    // First run, or the inserted card was removed by hand: whatever is in the
    // slot becomes a card of its own, so it is kept.
    const std::vector<fs::path> saves = SlotSaves();
    const std::string name = SuggestName();
    if (!WriteAll(m_library / (name + ".gci"), saves.empty() ? std::vector<char>{} : ReadAll(saves.front()),
                  error))
      return false;
    if (!saves.empty())
    {
      KeepTime(saves.front(), m_library / (name + ".gci"));
      CarryInfo(saves.front(), m_library / (name + ".gci"));
    }
    if (!List(error))
      return false;
    for (std::size_t i = 0; i < m_cards.size(); ++i)
      if (m_cards[i].name == name)
        m_inserted = static_cast<int>(i);
    if (!WriteInserted(error))
      return false;
  }
  return CollectSlot(error);
}

bool MemoryCardLibrary::CollectSlot(std::string* error)
{
  if (m_inserted < 0)
    return true;
  const LibraryCard& card = m_cards[static_cast<std::size_t>(m_inserted)];
  const std::vector<fs::path> saves = SlotSaves();
  const std::vector<char> slot = saves.empty() ? std::vector<char>{} : ReadAll(saves.front());
  const std::vector<char> stored = ReadAll(card.file);
  if (slot == stored)
  {
    if (!saves.empty())
    {
      std::error_code info_ec;
      if (fs::exists(InfoFor(saves.front()), info_ec))
      {
        std::ifstream a(InfoFor(saves.front())), b(InfoFor(card.file));
        const std::string sa{std::istreambuf_iterator<char>(a), {}},
            sb{std::istreambuf_iterator<char>(b), {}};
        if (sa != sb)
        {
          CarryInfo(saves.front(), card.file);
          return List(error);
        }
      }
      std::error_code ec;
      const auto slot_time = fs::last_write_time(saves.front(), ec);
      std::error_code card_ec;
      if (!ec && fs::last_write_time(card.file, card_ec) != slot_time && !card_ec)
      {
        KeepTime(saves.front(), card.file);
        return List(error);
      }
    }
    return true;
  }
  std::error_code ec;
  if (!stored.empty())
    fs::copy_file(card.file, fs::path(card.file).concat(".bak"),
                  fs::copy_options::overwrite_existing, ec);
  if (!WriteAll(card.file, slot, error))
    return false;
  if (!saves.empty())
    KeepTime(saves.front(), card.file);
  CarryInfo(saves.empty() ? fs::path() : saves.front(), card.file);
  return List(error);
}

bool MemoryCardLibrary::Insert(int index, std::string* error)
{
  if (index < 0 || index >= static_cast<int>(m_cards.size()))
    return false;
  if (index == m_inserted)
    return true;
  const std::string target = m_cards[static_cast<std::size_t>(index)].name;
  if (!CollectSlot(error))
    return false;
  std::error_code ec;
  fs::create_directories(m_slot, ec);
  for (const fs::path& save : SlotSaves())
  {
    fs::remove(InfoFor(save), ec);
    fs::remove(save, ec);
    if (ec)
    {
      if (error)
        *error = "can't remove " + save.string() + ": " + ec.message();
      return false;
    }
  }
  for (std::size_t i = 0; i < m_cards.size(); ++i)
    if (m_cards[i].name == target)
      m_inserted = static_cast<int>(i);
  const std::vector<char> data = ReadAll(m_cards[static_cast<std::size_t>(m_inserted)].file);
  if (!data.empty())
  {
    if (!WriteAll(m_slot / DolphinFileName(data), data, error))
      return false;
    KeepTime(m_cards[static_cast<std::size_t>(m_inserted)].file, m_slot / DolphinFileName(data));
    CarryInfo(m_cards[static_cast<std::size_t>(m_inserted)].file, m_slot / DolphinFileName(data));
  }
  return WriteInserted(error);
}

bool MemoryCardLibrary::Create(const std::string& name, std::string* error)
{
  if (!NameAvailable(name))
  {
    if (error)
      *error = "that card name is taken or not allowed";
    return false;
  }
  return WriteAll(m_library / (name + ".gci"), {}, error) && List(error);
}

bool MemoryCardLibrary::Duplicate(int index, const std::string& name, std::string* error)
{
  if (index < 0 || index >= static_cast<int>(m_cards.size()) || !NameAvailable(name))
  {
    if (error)
      *error = "that card name is taken or not allowed";
    return false;
  }
  if (index == m_inserted && !CollectSlot(error))
    return false;
  if (!WriteAll(m_library / (name + ".gci"), ReadAll(m_cards[static_cast<std::size_t>(index)].file),
                error))
    return false;
  KeepTime(m_cards[static_cast<std::size_t>(index)].file, m_library / (name + ".gci"));
  CarryInfo(m_cards[static_cast<std::size_t>(index)].file, m_library / (name + ".gci"));
  return List(error);
}

bool MemoryCardLibrary::Delete(int index,
                               const std::function<bool(const fs::path&)>& remove,
                               std::string* error)
{
  if (index < 0 || index >= static_cast<int>(m_cards.size()))
    return false;
  if (index == m_inserted)
  {
    if (error)
      *error = "put another card in Slot A before deleting this one";
    return false;
  }
  const fs::path file = m_cards[static_cast<std::size_t>(index)].file;
  if (!remove(file))
  {
    if (error)
      *error = "could not delete " + file.string();
    return false;
  }
  std::error_code ec;
  if (fs::exists(fs::path(file).concat(".bak"), ec))
    remove(fs::path(file).concat(".bak"));
  if (fs::exists(InfoFor(file), ec))
    remove(InfoFor(file));
  return List(error);
}

bool MemoryCardLibrary::Rename(int index, const std::string& name, std::string* error)
{
  if (index < 0 || index >= static_cast<int>(m_cards.size()))
    return false;
  LibraryCard& card = m_cards[static_cast<std::size_t>(index)];
  // A change of case only is allowed; anything else must be free.
  if (Lower(card.name) != Lower(name) && !NameAvailable(name))
  {
    if (error)
      *error = "that card name is taken or not allowed";
    return false;
  }
  if (!ValidName(name))
    return false;
  std::error_code ec;
  const fs::path target = m_library / (name + ".gci");
  const fs::path staging = m_library / (name + ".gci.renaming");
  fs::rename(card.file, staging, ec);
  if (!ec)
    fs::rename(staging, target, ec);
  if (ec)
  {
    if (error)
      *error = "can't rename " + card.file.string() + ": " + ec.message();
    return false;
  }
  if (fs::exists(fs::path(card.file).concat(".bak"), ec))
    fs::rename(fs::path(card.file).concat(".bak"), fs::path(target).concat(".bak"), ec);
  if (fs::exists(InfoFor(card.file), ec))
    fs::rename(InfoFor(card.file), InfoFor(target), ec);
  const bool inserted = index == m_inserted;
  card.name = name;
  card.file = target;
  if (!List(error))
    return false;
  if (inserted)
  {
    for (std::size_t i = 0; i < m_cards.size(); ++i)
      if (m_cards[i].name == name)
        m_inserted = static_cast<int>(i);
    return WriteInserted(error);
  }
  return true;
}
}  // namespace moderngekko::frontend
