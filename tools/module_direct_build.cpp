// SPDX-License-Identifier: GPL-2.0-or-later

#include "module_direct_build.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

namespace moderngekko::module_build
{
namespace
{
bool ReadText(const fs::path& path, std::string* text)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *text = buffer.str();
  return true;
}

bool WriteText(const fs::path& path, std::string_view text)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

bool IsHex(char c, bool upper_only)
{
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (!upper_only && c >= 'a' && c <= 'f');
}

// Parses hex digits at text[pos...]; returns the number of digits read (0 if none).
std::size_t ParseHex(std::string_view text, std::size_t pos, std::uint64_t* value,
                     bool upper_only = false, std::size_t max_digits = 16)
{
  std::size_t n = 0;
  std::uint64_t v = 0;
  while (pos + n < text.size() && n < max_digits && IsHex(text[pos + n], upper_only))
  {
    const char c = text[pos + n];
    v = v * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    ++n;
  }
  *value = v;
  return n;
}

bool IsSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::size_t SkipSpace(std::string_view text, std::size_t pos)
{
  while (pos < text.size() && IsSpace(text[pos]))
    ++pos;
  return pos;
}

// Matches `literal` at pos; returns the position after it or npos.
std::size_t Expect(std::string_view text, std::size_t pos, std::string_view literal)
{
  return text.substr(pos, literal.size()) == literal ? pos + literal.size() : std::string_view::npos;
}

// "0x" + hex + "u" at pos (gen_module_tables' (0x[0-9A-Fa-f]+)u).
std::size_t ExpectHexU(std::string_view text, std::size_t pos, std::uint64_t* value)
{
  pos = Expect(text, pos, "0x");
  if (pos == std::string_view::npos)
    return pos;
  const std::size_t digits = ParseHex(text, pos, value);
  if (digits == 0)
    return std::string_view::npos;
  return Expect(text, pos + digits, "u");
}

std::uint64_t Fnv1a64(const unsigned char* data, std::size_t size)
{
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (std::size_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 0x100000001B3ULL;
  return hash;
}

std::string Hex8(std::uint32_t value)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08X", value);
  return buffer;
}
}  // namespace

bool WriteModuleTables(const fs::path& generated_h, const fs::path& smc_txt, const fs::path& dol,
                       const fs::path& out_inc, std::string* error)
{
  std::string header;
  if (!ReadText(generated_h, &header))
  {
    *error = "cannot read " + generated_h.string();
    return false;
  }
  const std::string_view h(header);

  std::set<std::pair<std::uint32_t, std::uint32_t>> code_ranges;
  // Per-chunk address guards: address >= 0x...u && address < 0x...u
  for (std::size_t at = h.find("address >= "); at != std::string_view::npos;
       at = h.find("address >= ", at + 1))
  {
    std::uint64_t a = 0, b = 0;
    std::size_t p = ExpectHexU(h, at + 11, &a);
    if (p == std::string_view::npos || (p = Expect(h, p, " && address < ")) == std::string_view::npos ||
        ExpectHexU(h, p, &b) == std::string_view::npos)
      continue;
    code_ranges.emplace(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
  }
  // Offset-table dispatch runs: u32 offset = address - 0x...u; if (offset < 0x...u
  for (std::size_t at = h.find("u32"); at != std::string_view::npos; at = h.find("u32", at + 1))
  {
    std::size_t p = SkipSpace(h, at + 3);
    if (p == at + 3 || (p = Expect(h, p, "offset")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "=")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "address")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "-")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    std::uint64_t base = 0, span = 0;
    if ((p = ExpectHexU(h, p, &base)) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, ";")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "if")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "(")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "offset")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if ((p = Expect(h, p, "<")) == std::string_view::npos)
      continue;
    p = SkipSpace(h, p);
    if (ExpectHexU(h, p, &span) == std::string_view::npos)
      continue;
    code_ranges.emplace(static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base + span));
  }
  if (code_ranges.empty())
  {
    *error = "no coverage ranges found in " + generated_h.string();
    return false;
  }

  // void func_XXXXXXXX(CPUState* ctx);
  std::vector<std::uint32_t> funcs;
  for (std::size_t at = h.find("void func_"); at != std::string_view::npos;
       at = h.find("void func_", at + 1))
  {
    std::uint64_t address = 0;
    if (ParseHex(h, at + 10, &address, false, 8) == 8 &&
        Expect(h, at + 18, "(CPUState* ctx);") != std::string_view::npos)
      funcs.push_back(static_cast<std::uint32_t>(address));
  }
  std::sort(funcs.begin(), funcs.end());
  if (funcs.empty())
  {
    *error = "no func_ declarations found in " + generated_h.string();
    return false;
  }
  std::vector<std::pair<std::uint32_t, std::uint32_t>> chunk_ranges;
  for (std::size_t i = 0; i < funcs.size(); ++i)
  {
    const std::uint32_t address = funcs[i];
    const auto containing = std::find_if(code_ranges.begin(), code_ranges.end(), [&](auto& r) {
      return r.first <= address && address < r.second;
    });
    if (containing == code_ranges.end())
    {
      *error = "func_" + Hex8(address) + " outside all code ranges";
      return false;
    }
    std::uint32_t end = containing->second;
    if (i + 1 < funcs.size() && containing->first <= funcs[i + 1] && funcs[i + 1] < containing->second)
      end = funcs[i + 1];
    chunk_ranges.emplace_back(address, end);
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t>> smc_ranges;
  std::string smc;
  if (ReadText(smc_txt, &smc))
  {
    std::istringstream lines(smc);
    for (std::string line; std::getline(lines, line);)
    {
      const std::string_view l(line);
      const std::size_t p = SkipSpace(l, 0);
      std::uint64_t a = 0, b = 0;
      std::size_t q = Expect(l, p, "0x");
      if (q == std::string_view::npos)
        continue;
      const std::size_t da = ParseHex(l, q, &a);
      if (da == 0 || (q = Expect(l, q + da, "-0x")) == std::string_view::npos ||
          ParseHex(l, q, &b) == 0)
        continue;
      // File ranges are inclusive instruction addresses; ABI ranges are end-exclusive.
      smc_ranges.emplace_back(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b + 4));
    }
  }
  std::sort(smc_ranges.begin(), smc_ranges.end());

  std::string dol_bytes;
  if (!ReadText(dol, &dol_bytes) || dol_bytes.size() < 0x100)
  {
    *error = "cannot read " + dol.string();
    return false;
  }
  const auto be32 = [&](std::size_t off) {
    const auto* p = reinterpret_cast<const unsigned char*>(dol_bytes.data()) + off;
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
  };
  struct Section
  {
    std::uint32_t address, size, file_off;
  };
  std::vector<Section> sections;
  for (int i = 0; i < 18; ++i)
  {
    const std::uint32_t file_off = be32(i * 4), address = be32(0x48 + i * 4), size = be32(0x90 + i * 4);
    if (file_off && address && size)
      sections.push_back({address, size, file_off});
  }

  std::ostringstream out;
  out << "// Generated by moderngekko-port (gen_module_tables) -- do not edit.\n";
  char line[64];
  out << "static const StaticRecompRange s_code_ranges[] = {\n";
  for (const auto& [a, b] : code_ranges)
  {
    std::snprintf(line, sizeof(line), "    {0x%08Xu, 0x%08Xu},\n", a, b);
    out << line;
  }
  out << "};\n#define MODULE_CODE_RANGE_COUNT " << code_ranges.size() << "u\n";
  out << "static const StaticRecompRange s_smc_ranges[] = {\n";
  if (smc_ranges.empty())
    out << "    {0u, 0u}, /* C and MSVC do not support zero-sized arrays. */\n";
  for (const auto& [a, b] : smc_ranges)
  {
    std::snprintf(line, sizeof(line), "    {0x%08Xu, 0x%08Xu},\n", a, b);
    out << line;
  }
  out << "};\n#define MODULE_SMC_RANGE_COUNT " << smc_ranges.size() << "u\n";
  out << "static const StaticRecompRange s_chunk_ranges[] = {\n";
  for (const auto& [a, b] : chunk_ranges)
  {
    std::snprintf(line, sizeof(line), "    {0x%08Xu, 0x%08Xu},\n", a, b);
    out << line;
  }
  out << "};\n#define MODULE_CHUNK_RANGE_COUNT " << chunk_ranges.size() << "u\n";
  // FNV-1a 64 of each chunk's original text, so the chassis can verify that guest RAM still
  // holds the code this module was compiled from.
  out << "static const u64 s_chunk_hashes[] = {\n";
  for (const auto& [a, b] : chunk_ranges)
  {
    const auto section = std::find_if(sections.begin(), sections.end(), [&](const Section& s) {
      return s.address <= a && b <= s.address + s.size;
    });
    if (section == sections.end() || section->file_off + (b - section->address) > dol_bytes.size())
    {
      *error = "range [0x" + Hex8(a) + ",0x" + Hex8(b) + ") not inside one DOL section";
      return false;
    }
    const std::size_t lo = section->file_off + (a - section->address);
    std::snprintf(line, sizeof(line), "    0x%016llXu,\n",
                  static_cast<unsigned long long>(Fnv1a64(
                      reinterpret_cast<const unsigned char*>(dol_bytes.data()) + lo, b - a)));
    out << line;
  }
  out << "};\n";
  if (!WriteText(out_inc, out.str()))
  {
    *error = "cannot write " + out_inc.string();
    return false;
  }
  return true;
}

bool ReadHookList(const fs::path& path, std::vector<Hook>* hooks, std::string* error)
{
  std::string text;
  if (!ReadText(path, &text))
  {
    *error = "cannot read hook list " + path.string();
    return false;
  }
  std::istringstream lines(text);
  int number = 0;
  for (std::string line; std::getline(lines, line);)
  {
    ++number;
    if (const std::size_t hash = line.find('#'); hash != std::string::npos)
      line.resize(hash);
    std::istringstream fields(line);
    std::string address, size, flag;
    if (!(fields >> address))
      continue;
    Hook hook;
    std::uint64_t a = 0, s = 0;
    const std::string_view av = address.starts_with("0x") ? std::string_view(address).substr(2) :
                                                             std::string_view(address);
    const std::string_view sv_full = (fields >> size) ? std::string_view(size) : std::string_view();
    const std::string_view sv = sv_full.starts_with("0x") ? sv_full.substr(2) : sv_full;
    if (ParseHex(av, 0, &a) != av.size() || av.size() != 8 || sv.empty() ||
        ParseHex(sv, 0, &s) != sv.size())
    {
      *error = path.string() + ":" + std::to_string(number) + ": expected ADDRESS SIZE [r]";
      return false;
    }
    hook.address = static_cast<std::uint32_t>(a);
    hook.size = static_cast<std::uint32_t>(s);
    hook.has_return_hooks = (fields >> flag) && flag == "r";
    hooks->push_back(hook);
  }
  return true;
}

bool MakeHookAware(const fs::path& generated_dir, const std::vector<Hook>& hook_list,
                   const std::vector<std::uint32_t>& idle_continuations, std::string* error)
{
  // address -> (end, has return hooks), and each hook's live-flag index in address order.
  std::map<std::uint32_t, std::pair<std::uint32_t, bool>> hooks;
  for (const Hook& hook : hook_list)
  {
    auto& entry = hooks[hook.address];
    entry.first = hook.address + hook.size;
    entry.second = entry.second || hook.has_return_hooks;
  }
  std::map<std::uint32_t, std::size_t> order;
  for (const auto& [address, unused] : hooks)
    order.emplace(address, order.size());
  const auto live = [&](std::uint32_t address) {
    return "dolrecomp_hook_live[" + std::to_string(order.at(address)) + "]";
  };
  const auto in_hook = [&](std::uint32_t address) -> const std::pair<const std::uint32_t,
                                                                      std::pair<std::uint32_t, bool>>* {
    for (const auto& entry : hooks)
    {
      if (entry.first <= address && address < entry.second.first)
        return &entry;
    }
    return nullptr;
  };

  std::error_code ec;
  const fs::path chunks = generated_dir / "chunks";
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(chunks, ec))
  {
    if (entry.path().extension() == ".c" && entry.path().filename() != "zz_hook_aware_marker.c")
      files.push_back(entry.path());
  }
  if (ec)
  {
    *error = "cannot list " + chunks.string();
    return false;
  }
  std::sort(files.begin(), files.end());

  static constexpr std::string_view kEnter = "if (dolrecomp_call_enter()) {\n";
  for (const fs::path& file : files)
  {
    std::string original;
    if (!ReadText(file, &original))
    {
      *error = "cannot read " + file.string();
      return false;
    }
    // Universal newlines, as the Python tool read them.
    std::string text;
    text.reserve(original.size());
    for (std::size_t i = 0; i < original.size(); ++i)
    {
      if (original[i] == '\r' && i + 1 < original.size() && original[i + 1] == '\n')
        continue;
      text += original[i];
    }
    const std::string before = text;

    // Call sites: "ctx->pc = 0xTu;\n<ws>if (dolrecomp_call_enter()) {\n<ws>void func_T(...);"
    // into a hooked T go back through the dispatcher while the hook is live; idle-loop calls
    // ("ctx->lr = 0xCu;\n<ws>ctx->pc = ...;\n<ws>if (dolrecomp_call_enter())") always do.
    std::string rewritten;
    rewritten.reserve(text.size() + 1024);
    std::size_t copied = 0;
    for (std::size_t at = text.find(kEnter); at != std::string::npos; at = text.find(kEnter, at + 1))
    {
      const std::string_view t(text);
      // Back over the whitespace before "if": at least "\n" plus one more character.
      std::size_t ws = at;
      while (ws > 0 && IsSpace(t[ws - 1]))
        --ws;
      if (at - ws < 2 || t[ws] != '\n')
        continue;
      // "ctx->pc = 0xXXXXXXXXu;" ends at ws.
      static constexpr std::string_view kPc = "ctx->pc = 0x";
      if (ws < kPc.size() + 10)
        continue;
      const std::size_t pc_at = ws - (kPc.size() + 10);
      std::uint64_t target = 0;
      if (t.substr(pc_at, kPc.size()) != kPc ||
          ParseHex(t, pc_at + kPc.size(), &target, true, 8) != 8 ||
          t.substr(pc_at + kPc.size() + 8, 2) != "u;")
        continue;

      bool site = false;
      {
        std::size_t p = at + kEnter.size();
        const std::size_t indent = p;
        while (p < t.size() && IsSpace(t[p]))
          ++p;
        std::uint64_t callee = 0;
        site = p > indent && Expect(t, p, "void func_") != std::string_view::npos &&
               ParseHex(t, p + 10, &callee, true, 8) == 8 &&
               Expect(t, p + 18, "(CPUState* ctx);") != std::string_view::npos;
      }
      std::string replacement;
      if (site && hooks.contains(static_cast<std::uint32_t>(target)))
      {
        replacement = "if (!" + live(static_cast<std::uint32_t>(target)) + " && dolrecomp_call_enter())";
      }
      else
      {
        // ctx->lr line before the pc line: "ctx->lr = 0xXXXXXXXXu;\n<ws>"
        std::size_t lw = pc_at;
        while (lw > 0 && IsSpace(t[lw - 1]))
          --lw;
        static constexpr std::string_view kLr = "ctx->lr = 0x";
        if (pc_at - lw >= 2 && t[lw] == '\n' && lw >= kLr.size() + 10)
        {
          const std::size_t lr_at = lw - (kLr.size() + 10);
          std::uint64_t lr = 0;
          if (t.substr(lr_at, kLr.size()) == kLr &&
              ParseHex(t, lr_at + kLr.size(), &lr, false, 8) == 8 &&
              t.substr(lr_at + kLr.size() + 8, 2) == "u;" &&
              std::find(idle_continuations.begin(), idle_continuations.end(),
                        static_cast<std::uint32_t>(lr)) != idle_continuations.end())
            replacement = "if (0) /* idle-detect continuation */";
        }
      }
      if (replacement.empty())
        continue;
      rewritten.append(text, copied, at - copied);
      rewritten += replacement;
      copied = at + std::string_view("if (dolrecomp_call_enter())").size();
    }
    rewritten.append(text, copied, std::string::npos);
    text = std::move(rewritten);

    bool mentions_hook = false;
    for (const auto& [address, unused] : hooks)
    {
      if (text.find("label_" + Hex8(address) + ":") != std::string::npos ||
          text.find("goto label_" + Hex8(address) + ";") != std::string::npos)
      {
        mentions_hook = true;
        break;
      }
    }
    if (mentions_hook)
    {
      // Rewrites every exact "goto label_XXXXXXXX;" in line through fn(target) -> replacement
      // (empty = keep).
      const auto rewrite_gotos = [](const std::string& line, const auto& fn) {
        static constexpr std::string_view kGoto = "goto label_";
        std::string out;
        std::size_t copied_to = 0;
        for (std::size_t g = line.find(kGoto); g != std::string::npos; g = line.find(kGoto, g + 1))
        {
          std::uint64_t target = 0;
          if (ParseHex(line, g + kGoto.size(), &target, true, 8) != 8 ||
              line.compare(g + kGoto.size() + 8, 1, ";") != 0)
            continue;
          const std::string replacement = fn(static_cast<std::uint32_t>(target));
          if (replacement.empty())
            continue;
          out.append(line, copied_to, g - copied_to);
          out += replacement;
          copied_to = g + kGoto.size() + 9;
        }
        out.append(line, copied_to, std::string::npos);
        return out;
      };
      const auto goto_targets = [](const std::string& line) {
        std::vector<std::uint32_t> targets;
        static constexpr std::string_view kGoto = "goto label_";
        for (std::size_t g = line.find(kGoto); g != std::string::npos; g = line.find(kGoto, g + 1))
        {
          std::uint64_t target = 0;
          if (ParseHex(line, g + kGoto.size(), &target, true, 8) == 8 &&
              line.compare(g + kGoto.size() + 8, 1, ";") == 0)
            targets.push_back(static_cast<std::uint32_t>(target));
        }
        return targets;
      };

      std::string out;
      out.reserve(text.size() + 4096);
      bool in_main = false, entry_switch_done = false, have_current = false;
      std::uint32_t current = 0;
      std::size_t start = 0;
      while (start <= text.size())
      {
        std::size_t end = text.find('\n', start);
        const bool last = end == std::string::npos;
        if (last)
          end = text.size();
        std::string line = text.substr(start, end - start);
        if (line.starts_with("void func_"))
        {
          in_main = true;
          have_current = false;
          entry_switch_done = false;
        }
        else if (line.starts_with("static void loop_"))
        {
          in_main = false;
        }
        std::uint64_t label = 0;
        if (line.starts_with("label_") && ParseHex(line, 6, &label, true, 8) == 8 &&
            line.compare(14, 1, ":") == 0)
        {
          current = static_cast<std::uint32_t>(label);
          have_current = true;
          entry_switch_done = true;
        }
        if (in_main)
        {
          const auto* region = have_current ? in_hook(current) : nullptr;
          // Same-chunk jumps to a hooked entry go through the chassis (not the entry switch's
          // own cases, and not a hooked function's back-edges to its own entry).
          const auto targets = goto_targets(line);
          const bool own_entry = region && std::find(targets.begin(), targets.end(),
                                                     region->first) != targets.end();
          if (!own_entry)
          {
            const std::size_t first = line.find_first_not_of(" \t\n\r\v\f");
            const bool entry_case = first != std::string::npos &&
                                    line.compare(first, 5, "case ") == 0 && !entry_switch_done;
            line = rewrite_gotos(line, [&](std::uint32_t target) -> std::string {
              if (!hooks.contains(target) || entry_case)
                return {};
              return "{ if (" + live(target) + ") { ctx->pc = 0x" + Hex8(target) +
                     "u; return; } goto label_" + Hex8(target) + " /*ha*/; }";
            });
          }
          if (region)
          {
            const std::uint32_t h = region->first, end_address = region->second.first;
            const bool ret = region->second.second;
            if (ret && line.find("goto return_dispatch_") != std::string::npos)
            {
              static constexpr std::string_view kRet = "goto return_dispatch_";
              std::string replaced;
              std::size_t c = 0;
              for (std::size_t g = line.find(kRet); g != std::string::npos; g = line.find(kRet, g + 1))
              {
                std::uint64_t r = 0;
                if (ParseHex(line, g + kRet.size(), &r, true, 8) != 8 ||
                    line.compare(g + kRet.size() + 8, 1, ";") != 0)
                  continue;
                replaced.append(line, c, g - c);
                replaced += "{ if (" + live(h) + ") return; goto return_dispatch_" +
                            Hex8(static_cast<std::uint32_t>(r)) + " /*ha*/; }";
                c = g + kRet.size() + 9;
              }
              replaced.append(line, c, std::string::npos);
              line = std::move(replaced);
            }
            line = rewrite_gotos(line, [&](std::uint32_t target) -> std::string {
              if (!ret || (h <= target && target < end_address))
                return {};
              return "{ if (" + live(h) + ") { ctx->pc = 0x" + Hex8(target) +
                     "u; return; } goto label_" + Hex8(target) + " /*ha*/; }";
            });
          }
        }
        out += line;
        if (last)
          break;
        out += '\n';
        start = end + 1;
      }
      text = std::move(out);
    }
    if (text != before && !WriteText(file, "extern unsigned char dolrecomp_hook_live[];\n" + text))
    {
      *error = "cannot write " + file.string();
      return false;
    }
  }

  std::string addresses, flags;
  for (const auto& [address, unused] : hooks)
  {
    addresses += "0x" + Hex8(address) + "u, ";
    flags += flags.empty() ? "1" : ", 1";
  }
  const std::string marker =
      "/* Marks this module as hook-aware and lists the hooked addresses; see\n"
      "   module_direct_build.cpp. */\n"
      "#ifdef _WIN32\n"
      "#define HOOK_AWARE_EXPORT __declspec(dllexport)\n"
      "#else\n"
      "#define HOOK_AWARE_EXPORT __attribute__((visibility(\"default\")))\n"
      "#endif\n"
      "HOOK_AWARE_EXPORT const int dolrecomp_hook_aware_calls = 1;\n"
      "HOOK_AWARE_EXPORT const unsigned int dolrecomp_hook_aware_addresses[] = {" +
      addresses + "0u};\n"
      "/* Per hook, in address order: 1 = a loaded mod uses it (go through the\n"
      "   dispatcher). The runtime clears unused ones; all 1 is always correct. */\n"
      "HOOK_AWARE_EXPORT unsigned char dolrecomp_hook_live[" +
      std::to_string(std::max<std::size_t>(hooks.size(), 1)) + "] = {" +
      (flags.empty() ? std::string("0") : flags) + "};\n";
  if (!WriteText(chunks / "zz_hook_aware_marker.c", marker))
  {
    *error = "cannot write the hook-aware marker";
    return false;
  }
  return true;
}

std::string HostZigTarget()
{
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
  return "aarch64-windows-gnu";
#elif defined(_WIN32)
  return "x86_64-windows-gnu";
#elif defined(__APPLE__) && defined(__aarch64__)
  return "aarch64-macos";
#elif defined(__APPLE__)
  return "x86_64-macos";
#elif defined(__aarch64__)
  return "aarch64-linux-gnu";
#else
  return "x86_64-linux-gnu";
#endif
}

namespace
{
// Runs a program with arguments, its output to log; true on exit code 0.
bool RunLogged(const std::vector<std::string>& args, const fs::path& log)
{
#if defined(_WIN32)
  // Windows command-line quoting (CommandLineToArgvW rules).
  std::wstring command_line;
  for (const std::string& arg : args)
  {
    const std::wstring w = fs::path(arg).wstring();
    if (!command_line.empty())
      command_line += L' ';
    command_line += L'"';
    std::size_t backslashes = 0;
    for (wchar_t c : w)
    {
      if (c == L'\\')
      {
        ++backslashes;
        continue;
      }
      if (c == L'"')
        command_line.append(backslashes * 2 + 1, L'\\');
      else
        command_line.append(backslashes, L'\\');
      backslashes = 0;
      command_line += c;
    }
    command_line.append(backslashes * 2, L'\\');
    command_line += L'"';
  }
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE)
    return false;
  {
    const std::string header = fs::path(command_line).string() + "\n";
    DWORD written = 0;
    WriteFile(out, header.data(), static_cast<DWORD>(header.size()), &written, nullptr);
  }
  // Many of these run at once: each child inherits only its own log handle, not the other
  // jobs' (zig fails to start its clang child with "Unexpected" when it inherits strays).
  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  std::vector<unsigned char> attribute_storage(attribute_size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  HANDLE inherit_only[] = {out};
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_only,
                                 sizeof(inherit_only), nullptr, nullptr))
  {
    CloseHandle(out);
    return false;
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullptr;
  startup.StartupInfo.hStdOutput = out;
  startup.StartupInfo.hStdError = out;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.push_back(L'\0');
  const BOOL started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                      nullptr, &startup.StartupInfo, &process);
  DeleteProcThreadAttributeList(attributes);
  CloseHandle(out);
  if (!started)
    return false;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exit_code != 0)
  {
    std::ofstream(log, std::ios::app) << "\n[exit code 0x" << std::hex << exit_code << "]\n";
  }
  return exit_code == 0;
#else
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  std::vector<char*> argv;
  for (const std::string& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0)
    return false;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
  {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::string Tail(const fs::path& log)
{
  std::string text;
  ReadText(log, &text);
  return text.size() > 3000 ? text.substr(text.size() - 3000) : text;
}

void SetEnv(const char* name, const std::string& value)
{
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}
}  // namespace

bool CompileModule(const CompileJob& job, std::string* error)
{
  std::error_code ec;
  fs::create_directories(job.work_dir / "obj", ec);
  const fs::path tables = job.work_dir / "module_tables.inc";
  if (!WriteModuleTables(job.generated_dir / "generated.h", job.generated_dir / "generated_smc.txt",
                         job.generated_dir / "main.dol", tables, error))
    return false;

  std::vector<fs::path> sources = {job.template_dir / "module_export.c",
                                   job.template_dir / "sdk_leaf_guard.c",
                                   job.template_dir / "native_call_guard.c"};
  for (const char* name : {"cpu.c", "native_sdk.c", "cpu_exception.c", "cpu_interpreter.c",
                           "cpu_interpreter_table.c", "cpu_interpreter_float.c",
                           "cpu_interpreter_integer.c"})
    sources.push_back(job.gxruntime_dir / "src" / "core" / name);
  std::vector<fs::path> chunks;
  for (const auto& entry : fs::directory_iterator(job.generated_dir / "chunks", ec))
  {
    if (entry.path().extension() == ".c")
      chunks.push_back(entry.path());
  }
  std::sort(chunks.begin(), chunks.end());
  if (chunks.empty())
  {
    *error = "DolRecomp output has no generated chunks in " + (job.generated_dir / "chunks").string();
    return false;
  }
  sources.insert(sources.end(), chunks.begin(), chunks.end());

  // Zig keeps its own caches; keep them with this build instead of the user profile.
  SetEnv("ZIG_GLOBAL_CACHE_DIR", (job.work_dir / "zig-cache").string());
  SetEnv("ZIG_LOCAL_CACHE_DIR", (job.work_dir / "zig-cache").string());

  const std::string target = HostZigTarget();
  const bool windows = target.find("windows") != std::string::npos;
  // ThinLTO needs LLD, which zig uses for ELF and COFF but not for Mach-O.
  const bool lto = target.find("macos") == std::string::npos;
  std::vector<std::string> common = {job.zig.string(), "cc", "-target", target, "-std=gnu11",
                                     "-O" + job.opt_level, "-ffp-contract=off", "-fno-fast-math",
                                     "-DMODULE_GAME_ID=\"" + job.game_id + "\"",
                                     "-Dg" + job.game_id + "_recomp_EXPORTS"};
  if (lto)
    common.push_back("-flto=thin");
  if (!windows)
  {
    common.push_back("-fvisibility=hidden");
    common.push_back("-fPIC");
  }
  for (const std::string& define : job.extra_defines)
    common.push_back("-D" + define);
  // As module-template's RECOMPCORE_MODULE_ARCH_FLAGS; -ffp-contract=off above
  // still stops the compiler fusing anything the code does not write as fma.
  common.insert(common.end(), job.arch_flags.begin(), job.arch_flags.end());
  if (fs::exists(job.generated_dir / "native_call_nofallback.inc"))
    common.push_back("-DDOLRECOMP_CALL_GUARD_MANIFEST=1");
  for (const fs::path& include : {job.generated_dir, job.gxruntime_dir / "include",
                                  job.chassis_abi_dir, job.work_dir, job.template_dir})
    common.push_back("-I" + include.string());

  const unsigned total = static_cast<unsigned>(sources.size()) + 1;  // + the link
  // At most 16 at once: past that zig's shared cache races more and the link dominates anyway.
  const unsigned jobs =
      std::clamp(job.jobs ? job.jobs : std::thread::hardware_concurrency(), 1u, 16u);
  std::vector<fs::path> objects(sources.size());
  std::atomic<std::size_t> next{0};
  std::atomic<unsigned> done{0};
  std::atomic<bool> failed{false};
  std::mutex error_mutex;
  std::mutex progress_mutex;
  const auto worker = [&] {
    for (std::size_t i = next++; i < sources.size() && !failed; i = next++)
    {
      const fs::path& source = sources[i];
      objects[i] = job.work_dir / "obj" /
                   (std::to_string(i) + "_" + source.stem().string() + ".o");
      const fs::path log = fs::path(objects[i]).replace_extension(".log");
      std::vector<std::string> args = common;
      args.insert(args.end(), {"-c", source.string(), "-o", objects[i].string()});
      // Many zig processes starting at once occasionally lose a race in its cache and exit
      // with no output; a retry compiles the file normally. A real error fails every time.
      bool compiled = false;
      for (int attempt = 0; attempt < 3 && !compiled && !failed; ++attempt)
        compiled = RunLogged(args, log);
      if (!compiled)
      {
        std::lock_guard lock(error_mutex);
        if (!failed.exchange(true))
          *error = "compiling " + source.filename().string() + " failed:\n" + Tail(log);
        return;
      }
      const unsigned n = ++done;
      if (job.progress)
      {
        std::lock_guard lock(progress_mutex);
        job.progress(n, total, "compiled " + source.filename().string());
      }
    }
  };
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < std::min<std::size_t>(jobs, sources.size()); ++t)
    threads.emplace_back(worker);
  for (std::thread& thread : threads)
    thread.join();
  if (failed)
    return false;

  if (job.progress)
    job.progress(total - 1, total, "linking");
  const fs::path rsp = job.work_dir / "link.rsp";
  {
    std::string list;
    for (const fs::path& object : objects)
    {
      std::string path = object.generic_string();
      list += '"' + path + "\"\n";
    }
    if (!WriteText(rsp, list))
    {
      *error = "cannot write " + rsp.string();
      return false;
    }
  }
  std::vector<std::string> link = {job.zig.string(), "cc", "-target", target, "-shared",
                                   "-O" + job.opt_level, "-o", job.output.string(),
                                   "@" + rsp.string()};
  if (lto)
    link.push_back("-flto=thin");
  if (target.find("linux") != std::string::npos)
  {
    link.push_back("-Wl,--version-script=" + (job.template_dir / "module.exports").string());
    link.push_back("-lm");
  }
  const fs::path link_log = job.work_dir / "link.log";
  if (!RunLogged(link, link_log) || !fs::is_regular_file(job.output))
  {
    *error = "linking the module failed:\n" + Tail(link_log);
    return false;
  }
  if (job.progress)
    job.progress(total, total, "linked");
  return true;
}
}  // namespace moderngekko::module_build
