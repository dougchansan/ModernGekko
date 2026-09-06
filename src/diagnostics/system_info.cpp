#include "moderngekko/diagnostics_report.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define MODERNGEKKO_DIAG_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace moderngekko::diagnostics
{
namespace
{
std::string Trim(std::string text)
{
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

std::string HostArchitecture()
{
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__riscv)
  return "riscv";
#elif defined(__powerpc64__)
  return "ppc64";
#else
  return "unknown";
#endif
}

#if defined(MODERNGEKKO_DIAG_X86)
void CpuId(unsigned leaf, unsigned subleaf, unsigned regs[4])
{
#if defined(_MSC_VER)
  int out[4];
  __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (int i = 0; i < 4; ++i)
    regs[i] = static_cast<unsigned>(out[i]);
#else
  unsigned eax = 0;
  unsigned ebx = 0;
  unsigned ecx = 0;
  unsigned edx = 0;
  __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
  regs[0] = eax;
  regs[1] = ebx;
  regs[2] = ecx;
  regs[3] = edx;
#endif
}

std::vector<std::string> X86InstructionSets()
{
  std::vector<std::string> sets;
  unsigned regs[4] = {};
  CpuId(0, 0, regs);
  const unsigned max_leaf = regs[0];
  if (max_leaf >= 1)
  {
    CpuId(1, 0, regs);
    const unsigned ecx = regs[2];
    const unsigned edx = regs[3];
    if ((edx & (1u << 25)) != 0)
      sets.emplace_back("SSE");
    if ((edx & (1u << 26)) != 0)
      sets.emplace_back("SSE2");
    if ((ecx & (1u << 0)) != 0)
      sets.emplace_back("SSE3");
    if ((ecx & (1u << 9)) != 0)
      sets.emplace_back("SSSE3");
    if ((ecx & (1u << 19)) != 0)
      sets.emplace_back("SSE4.1");
    if ((ecx & (1u << 20)) != 0)
      sets.emplace_back("SSE4.2");
    if ((ecx & (1u << 12)) != 0)
      sets.emplace_back("FMA");
    if ((ecx & (1u << 28)) != 0)
      sets.emplace_back("AVX");
    if ((ecx & (1u << 23)) != 0)
      sets.emplace_back("POPCNT");
    if ((ecx & (1u << 25)) != 0)
      sets.emplace_back("AES");
  }
  if (max_leaf >= 7)
  {
    CpuId(7, 0, regs);
    const unsigned ebx = regs[1];
    if ((ebx & (1u << 3)) != 0)
      sets.emplace_back("BMI1");
    if ((ebx & (1u << 5)) != 0)
      sets.emplace_back("AVX2");
    if ((ebx & (1u << 8)) != 0)
      sets.emplace_back("BMI2");
    if ((ebx & (1u << 16)) != 0)
      sets.emplace_back("AVX512F");
  }
  CpuId(0x80000000u, 0, regs);
  if (regs[0] >= 0x80000001u)
  {
    CpuId(0x80000001u, 0, regs);
    if ((regs[2] & (1u << 5)) != 0)
      sets.emplace_back("LZCNT");
  }
  return sets;
}

std::string X86CpuModel()
{
  unsigned regs[4] = {};
  CpuId(0x80000000u, 0, regs);
  if (regs[0] < 0x80000004u)
    return {};
  char brand[49] = {};
  for (unsigned leaf = 0; leaf < 3; ++leaf)
  {
    CpuId(0x80000002u + leaf, 0, regs);
    std::memcpy(brand + leaf * 16, regs, 16);
  }
  return Trim(std::string(brand));
}
#endif

std::vector<std::string> InstructionSets()
{
#if defined(MODERNGEKKO_DIAG_X86)
  return X86InstructionSets();
#elif defined(__aarch64__) || defined(_M_ARM64)
  std::vector<std::string> sets{"NEON"};
#if defined(__ARM_FEATURE_CRC32)
  sets.emplace_back("CRC32");
#endif
#if defined(__ARM_FEATURE_CRYPTO)
  sets.emplace_back("CRYPTO");
#endif
  return sets;
#else
  return {};
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::string ReadFirstField(const std::filesystem::path& path, std::string_view key)
{
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line))
  {
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    if (Trim(line.substr(0, colon)) != key)
      continue;
    return Trim(line.substr(colon + 1));
  }
  return {};
}

int LinuxPhysicalCores()
{
  std::ifstream file("/proc/cpuinfo");
  std::string line;
  std::vector<std::pair<std::string, std::string>> seen;
  std::string physical_id;
  std::string core_id;
  const auto flush = [&]() {
    if (physical_id.empty() && core_id.empty())
      return;
    const std::pair<std::string, std::string> key{physical_id, core_id};
    if (std::find(seen.begin(), seen.end(), key) == seen.end())
      seen.push_back(key);
    physical_id.clear();
    core_id.clear();
  };
  while (std::getline(file, line))
  {
    if (line.empty())
    {
      flush();
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    const std::string key = Trim(line.substr(0, colon));
    if (key == "physical id")
      physical_id = Trim(line.substr(colon + 1));
    else if (key == "core id")
      core_id = Trim(line.substr(colon + 1));
  }
  flush();
  return static_cast<int>(seen.size());
}

std::uint64_t LinuxStatusField(std::string_view key)
{
  std::ifstream file("/proc/self/status");
  std::string line;
  while (std::getline(file, line))
  {
    if (line.rfind(key, 0) != 0)
      continue;
    std::uint64_t value = 0;
    std::istringstream stream(line.substr(key.size() + 1));
    std::string unit;
    stream >> value >> unit;
    return value * 1024;
  }
  return 0;
}
#endif
}  // namespace

SystemInfo CollectSystemInfo()
{
  SystemInfo info;
  info.architecture = HostArchitecture();
  info.os_architecture = info.architecture;
  info.instruction_sets = InstructionSets();
  info.logical_processors = static_cast<int>(std::thread::hardware_concurrency());

#if defined(MODERNGEKKO_DIAG_X86)
  info.cpu_model = X86CpuModel();
#endif

#if defined(_WIN32)
  info.os_name = "Windows";
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (::GlobalMemoryStatusEx(&memory) != 0)
    info.total_physical_memory_bytes = memory.ullTotalPhys;
  PROCESS_MEMORY_COUNTERS counters{};
  if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) != 0)
  {
    info.process_resident_bytes = counters.WorkingSetSize;
    info.process_peak_bytes = counters.PeakWorkingSetSize;
  }
  DWORD length = 0;
  ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  std::vector<std::uint8_t> buffer(length);
  if (length != 0 && ::GetLogicalProcessorInformationEx(
                         RelationProcessorCore,
                         reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                             buffer.data()),
                         &length) != 0)
  {
    DWORD offset = 0;
    while (offset < length)
    {
      const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
          buffer.data() + offset);
      if (entry->Size == 0)
        break;
      ++info.physical_cores;
      offset += entry->Size;
    }
  }
  OSVERSIONINFOEXW version{};
  version.dwOSVersionInfoSize = sizeof(version);
#pragma warning(suppress : 4996)
  if (::GetVersionExW(reinterpret_cast<LPOSVERSIONINFOW>(&version)) != 0)
  {
    info.os_version = std::to_string(version.dwMajorVersion) + '.' +
                      std::to_string(version.dwMinorVersion) + '.' +
                      std::to_string(version.dwBuildNumber);
  }
#elif defined(__APPLE__)
  info.os_name = "macOS";
  auto sysctl_string = [](const char* name) {
    std::size_t size = 0;
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
      return std::string{};
    std::string value(size, '\0');
    if (::sysctlbyname(name, value.data(), &size, nullptr, 0) != 0)
      return std::string{};
    while (!value.empty() && value.back() == '\0')
      value.pop_back();
    return value;
  };
  auto sysctl_int = [](const char* name) -> std::uint64_t {
    std::uint64_t value = 0;
    std::size_t size = sizeof(value);
    if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0)
      return 0;
    return value;
  };
  if (info.cpu_model.empty())
    info.cpu_model = sysctl_string("machdep.cpu.brand_string");
  info.os_version = sysctl_string("kern.osproductversion");
  if (info.os_version.empty())
    info.os_version = sysctl_string("kern.osrelease");
  info.total_physical_memory_bytes = sysctl_int("hw.memsize");
  info.physical_cores = static_cast<int>(sysctl_int("hw.physicalcpu"));
  mach_task_basic_info task_info_data{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&task_info_data), &count) == KERN_SUCCESS)
  {
    info.process_resident_bytes = task_info_data.resident_size;
    info.process_peak_bytes = task_info_data.resident_size_max;
  }
#else
  info.os_name = "Linux";
  utsname uts{};
  if (::uname(&uts) == 0)
  {
    info.os_name = uts.sysname;
    info.os_version = uts.release;
    info.os_architecture = uts.machine;
  }
  if (info.cpu_model.empty())
    info.cpu_model = ReadFirstField("/proc/cpuinfo", "model name");
  if (info.cpu_model.empty())
    info.cpu_model = ReadFirstField("/proc/cpuinfo", "Hardware");
  info.physical_cores = LinuxPhysicalCores();
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0)
  {
    info.total_physical_memory_bytes =
        static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
  }
  info.process_resident_bytes = LinuxStatusField("VmRSS");
  info.process_peak_bytes = LinuxStatusField("VmHWM");
#endif

  if (info.physical_cores <= 0)
    info.physical_cores = info.logical_processors;
  if (info.cpu_model.empty())
    info.cpu_model = "unknown";
  if (info.os_name.empty())
    info.os_name = "unknown";
  return info;
}
}  // namespace moderngekko::diagnostics

namespace moderngekko::diagnostics
{
namespace
{
std::string CompilerName()
{
#if defined(__clang__)
  return "clang";
#elif defined(_MSC_VER)
  return "msvc";
#elif defined(__GNUC__)
  return "gcc";
#else
  return "unknown";
#endif
}

std::string CompilerVersion()
{
#if defined(__clang__)
  return std::to_string(__clang_major__) + '.' + std::to_string(__clang_minor__) + '.' +
         std::to_string(__clang_patchlevel__);
#elif defined(_MSC_VER)
  return std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
  return std::to_string(__GNUC__) + '.' + std::to_string(__GNUC_MINOR__) + '.' +
         std::to_string(__GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}
}  // namespace

BuildInfo CurrentBuildInfo()
{
  BuildInfo info;
#ifdef MODERNGEKKO_BUILD_VERSION
  info.version = MODERNGEKKO_BUILD_VERSION;
#endif
#ifdef MODERNGEKKO_BUILD_GIT_COMMIT
  info.git_commit = MODERNGEKKO_BUILD_GIT_COMMIT;
#endif
#ifdef MODERNGEKKO_BUILD_GIT_STATE
  info.git_state = MODERNGEKKO_BUILD_GIT_STATE;
#endif
#ifdef MODERNGEKKO_BUILD_CONFIGURATION
  info.configuration = MODERNGEKKO_BUILD_CONFIGURATION;
#endif
#ifdef MODERNGEKKO_BUILD_TIMESTAMP
  info.build_timestamp = MODERNGEKKO_BUILD_TIMESTAMP;
#endif
#ifdef MODERNGEKKO_BUILD_LTO
  info.link_time_optimization = MODERNGEKKO_BUILD_LTO != 0;
#endif
#ifdef MODERNGEKKO_BUILD_CPU_BACKEND
  info.cpu_backend = MODERNGEKKO_BUILD_CPU_BACKEND;
#endif
  if (info.version.empty())
    info.version = "unknown";
  if (info.git_commit.empty())
    info.git_commit = "unknown";
  if (info.configuration.empty())
    info.configuration = "unknown";
  if (info.cpu_backend.empty())
    info.cpu_backend = "StaticRecomp";
  info.compiler = CompilerName();
  info.compiler_version = CompilerVersion();
  info.host_architecture = HostArchitecture();
  return info;
}
}  // namespace moderngekko::diagnostics
