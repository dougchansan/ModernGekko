#include "diagnostics_internal.hpp"

#include <array>
#include <cstring>
#include <fstream>

namespace moderngekko::diagnostics::detail
{
namespace
{
constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50u;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50u;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50u;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kEndOfCentralDirectorySize = 22;

void PutU16(std::string& out, std::uint16_t value)
{
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void PutU32(std::string& out, std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

std::uint16_t GetU16(std::string_view data, std::size_t offset)
{
  return static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset])) |
         static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset + 1]) << 8);
}

std::uint32_t GetU32(std::string_view data, std::size_t offset)
{
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i)
    value = (value << 8) | static_cast<unsigned char>(data[offset + static_cast<std::size_t>(i)]);
  return value;
}

const std::array<std::uint32_t, 256>& CrcTable()
{
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> generated{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit)
        value = (value & 1u) != 0 ? (value >> 1) ^ 0xEDB88320u : value >> 1;
      generated[i] = value;
    }
    return generated;
  }();
  return table;
}
}  // namespace

std::uint32_t Crc32(std::string_view data)
{
  const auto& table = CrcTable();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char raw : data)
  {
    const auto index = static_cast<unsigned char>(crc ^ static_cast<unsigned char>(raw));
    crc = table[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

bool WriteArchive(const std::filesystem::path& path, const std::vector<ArchiveEntry>& entries,
                  std::string* error)
{
  std::string body;
  std::string directory;
  std::size_t total = 0;
  for (const ArchiveEntry& entry : entries)
    total += entry.data.size() + entry.name.size() * 2 + kLocalHeaderSize + kCentralHeaderSize;
  body.reserve(total);

  for (const ArchiveEntry& entry : entries)
  {
    if (entry.data.size() > 0xFFFFFFFFull || entry.name.size() > 0xFFFF)
    {
      if (error != nullptr)
        *error = "diagnostics entry '" + entry.name + "' is too large for the archive format";
      return false;
    }
    const auto offset = static_cast<std::uint32_t>(body.size());
    const std::uint32_t crc = Crc32(entry.data);
    const auto size = static_cast<std::uint32_t>(entry.data.size());
    const auto name_length = static_cast<std::uint16_t>(entry.name.size());

    PutU32(body, kLocalHeaderSignature);
    PutU16(body, 20);  // Version needed to extract.
    PutU16(body, 0);   // Flags.
    PutU16(body, 0);   // Stored.
    PutU16(body, 0);   // DOS time; fixed so reports are byte-reproducible.
    PutU16(body, 0x21);  // DOS date: 1980-01-01.
    PutU32(body, crc);
    PutU32(body, size);
    PutU32(body, size);
    PutU16(body, name_length);
    PutU16(body, 0);
    body += entry.name;
    body += entry.data;

    PutU32(directory, kCentralHeaderSignature);
    PutU16(directory, 20);
    PutU16(directory, 20);
    PutU16(directory, 0);
    PutU16(directory, 0);
    PutU16(directory, 0);
    PutU16(directory, 0x21);
    PutU32(directory, crc);
    PutU32(directory, size);
    PutU32(directory, size);
    PutU16(directory, name_length);
    PutU16(directory, 0);
    PutU16(directory, 0);
    PutU16(directory, 0);
    PutU16(directory, 0);
    PutU32(directory, 0);
    PutU32(directory, offset);
    directory += entry.name;
  }

  const auto directory_offset = static_cast<std::uint32_t>(body.size());
  const auto entry_count = static_cast<std::uint16_t>(entries.size());
  body += directory;
  PutU32(body, kEndOfCentralDirectorySignature);
  PutU16(body, 0);
  PutU16(body, 0);
  PutU16(body, entry_count);
  PutU16(body, entry_count);
  PutU32(body, static_cast<std::uint32_t>(directory.size()));
  PutU32(body, directory_offset);
  PutU16(body, 0);

  std::error_code ec;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
  {
    if (error != nullptr)
      *error = "could not open " + path.string() + " for writing";
    return false;
  }
  file.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!file)
  {
    if (error != nullptr)
      *error = "could not write " + path.string();
    return false;
  }
  return true;
}

bool ReadArchive(const std::filesystem::path& path, std::vector<ArchiveEntry>* entries,
                 std::string* error)
{
  const auto fail = [&](std::string message) {
    if (error != nullptr)
      *error = std::move(message);
    return false;
  };

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return fail("could not open " + path.string());
  const auto size = file.tellg();
  if (size < static_cast<std::streamoff>(kEndOfCentralDirectorySize))
    return fail(path.string() + " is not a diagnostics archive");
  std::string data(static_cast<std::size_t>(size), '\0');
  file.seekg(0);
  if (!file.read(data.data(), size))
    return fail("could not read " + path.string());

  std::size_t eocd = std::string::npos;
  const std::size_t search_limit =
      data.size() > 0x10000 + kEndOfCentralDirectorySize ? data.size() - 0x10000 : 0;
  for (std::size_t i = data.size() - kEndOfCentralDirectorySize + 1; i-- > search_limit;)
  {
    if (GetU32(data, i) == kEndOfCentralDirectorySignature)
    {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos)
    return fail(path.string() + " is not a diagnostics archive (no central directory)");

  const std::uint16_t count = GetU16(data, eocd + 10);
  const std::uint32_t directory_size = GetU32(data, eocd + 12);
  const std::uint32_t directory_offset = GetU32(data, eocd + 16);
  if (static_cast<std::size_t>(directory_offset) + directory_size > data.size())
    return fail(path.string() + " has a truncated central directory");

  std::vector<ArchiveEntry> result;
  result.reserve(count);
  std::size_t cursor = directory_offset;
  for (std::uint16_t i = 0; i < count; ++i)
  {
    if (cursor + kCentralHeaderSize > data.size() || GetU32(data, cursor) != kCentralHeaderSignature)
      return fail(path.string() + " has a malformed central directory entry");
    const std::uint16_t method = GetU16(data, cursor + 10);
    const std::uint32_t entry_size = GetU32(data, cursor + 24);
    const std::uint16_t name_length = GetU16(data, cursor + 28);
    const std::uint16_t extra_length = GetU16(data, cursor + 30);
    const std::uint16_t comment_length = GetU16(data, cursor + 32);
    const std::uint32_t local_offset = GetU32(data, cursor + 42);
    if (method != 0)
      return fail(path.string() + " uses an unsupported compression method");
    std::string name = data.substr(cursor + kCentralHeaderSize, name_length);

    if (static_cast<std::size_t>(local_offset) + kLocalHeaderSize > data.size() ||
        GetU32(data, local_offset) != kLocalHeaderSignature)
      return fail(path.string() + " has a malformed entry header for " + name);
    const std::uint16_t local_name_length = GetU16(data, local_offset + 26);
    const std::uint16_t local_extra_length = GetU16(data, local_offset + 28);
    const std::size_t start =
        local_offset + kLocalHeaderSize + local_name_length + local_extra_length;
    if (start + entry_size > data.size())
      return fail(path.string() + " has a truncated entry: " + name);
    std::string payload = data.substr(start, entry_size);
    if (Crc32(payload) != GetU32(data, cursor + 16))
      return fail(path.string() + " failed its checksum for " + name);
    result.push_back(ArchiveEntry{std::move(name), std::move(payload)});
    cursor += kCentralHeaderSize + name_length + extra_length + comment_length;
  }

  if (entries != nullptr)
    *entries = std::move(result);
  return true;
}
}  // namespace moderngekko::diagnostics::detail
