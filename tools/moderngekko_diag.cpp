// moderngekko-diag inspects and compares .mgdiag performance reports.

#include "moderngekko/diagnostics_report.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace
{
#ifndef MODERNGEKKO_DIAG_NAME
#define MODERNGEKKO_DIAG_NAME "moderngekko-diag"
#endif

void Usage()
{
  std::cerr << "usage: " MODERNGEKKO_DIAG_NAME " <command> [arguments]\n"
               "\n"
               "commands:\n"
               "  info <report.mgdiag>                    identity of the capture\n"
               "  summarize <report.mgdiag>               frame statistics and verdict\n"
               "  compare <report-a.mgdiag> <report-b.mgdiag>\n"
               "                                          side-by-side comparison\n";
}

bool Load(const std::string& path, moderngekko::diagnostics::Report* out)
{
  const moderngekko::diagnostics::ReadResult result =
      moderngekko::diagnostics::ReadReport(path);
  if (!result.ok)
  {
    std::cerr << "could not read " << path << ": " << result.error << '\n';
    return false;
  }
  *out = result.report;
  return true;
}
}  // namespace

int RunMain(int argc, char** argv)
{
  if (argc < 2)
  {
    Usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "--help" || command == "-h" || command == "help")
  {
    Usage();
    return 0;
  }

  if (command == "info" || command == "summarize")
  {
    if (argc != 3)
    {
      Usage();
      return 2;
    }
    moderngekko::diagnostics::Report report;
    if (!Load(argv[2], &report))
      return 1;
    std::cout << (command == "info" ? moderngekko::diagnostics::RenderInfo(report)
                                    : moderngekko::diagnostics::RenderSummary(report));
    return 0;
  }

  if (command == "compare")
  {
    if (argc != 4)
    {
      Usage();
      return 2;
    }
    moderngekko::diagnostics::Report a;
    moderngekko::diagnostics::Report b;
    if (!Load(argv[2], &a) || !Load(argv[3], &b))
      return 1;
    const moderngekko::diagnostics::Comparison comparison =
        moderngekko::diagnostics::CompareReports(a, b);
    std::cout << moderngekko::diagnostics::RenderComparison(a, b, comparison);
    // A mismatched pair is still printed, but the exit status flags it so
    // scripts do not silently compare unrelated captures.
    return comparison.mismatches.empty() ? 0 : 3;
  }

  std::cerr << "unknown command: " << command << '\n';
  Usage();
  return 2;
}

int main(int argc, char** argv)
{
  try
  {
    return RunMain(argc, argv);
  }
  catch (const std::exception& error)
  {
    std::cerr << "fatal error: " << error.what() << '\n';
  }
  catch (...)
  {
    std::cerr << "fatal error: unknown exception\n";
  }
  return 1;
}
