#include <exception>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/commands.hpp"
#include "seq/exit_codes.hpp"
#include "seq/version.hpp"
#include "support/config.hpp"
#include "support/process.hpp"

namespace {

void PrintHelp(std::ostream& out) {
  out << "Seq Compiler\n"
         "\n"
         "Usage:\n"
         "  seqc new <name>            Create a project in ./<name>.\n"
         "  seqc <file.seq>            Validate, build or reuse the cached "
         "build, run,\n"
         "                             and list the results.\n"
         "  seqc check <file.seq>      Parse and validate only.\n"
         "  seqc doctor                Check the prerequisites on this host.\n"
         "  seqc model pull [--update] Download and lock the project's "
         "model.\n"
         "  seqc clean [--all]         Remove run records; --all also removes "
         "build\n"
         "                             artifacts. Outputs and inputs are "
         "kept.\n"
         "  seqc --version             Show version information.\n"
         "  seqc --help                Show this message.\n"
         "\n"
         "Options for seqc <file.seq>:\n"
         "  --build-only   Stop after a successful build.\n"
         "  --rebuild      Ignore the build cache and synthesize again.\n"
         "  --force        Replace outputs that already exist and were not "
         "created\n"
         "                 by seqc, or were edited since.\n"
         "  --quiet        Print only errors, program output, and the "
         "results.\n"
         "\n"
         "Options for every command:\n"
         "  --set <section.key>=<value>   Override a setting for this run.\n"
         "  --settings                    List the settings and their "
         "defaults.\n"
         "\n"
         "Settings are read from flags, then SEQC_<SECTION>_<KEY> "
         "environment\n"
         "variables, then "
      << seq::Config::ConfigFilePath().generic_string()
      << ",\nthen built-in defaults. The entry point of a project is "
         "src/main.seq.\n";
}

void PrintVersion(std::ostream& out) {
  out << "seqc " << seq::kVersion << "\n"
      << "commit: " << seq::kGitCommit << "\n"
      << "grammar version: " << seq::kGrammarVersion << "\n"
      << "prompt version: " << seq::kPromptVersion << "\n";
}

void PrintSettings(std::ostream& out) {
  out << "Settings (section.key = default):\n\n";
  for (const seq::Setting& setting : seq::AllSettings()) {
    out << "  " << setting.key << " = \"" << setting.default_value << "\"\n"
        << "      " << setting.description << "\n";
  }
}

int Usage(const std::string& message) {
  std::cerr << "seqc: error: " << message << "\n"
            << "Run 'seqc --help' for usage.\n";
  return seq::kExitUsage;
}

int Run(int argc, char* argv[]) {
  std::vector<std::string> positional;
  std::vector<std::string> overrides;
  bool build_only = false;
  bool rebuild = false;
  bool force = false;
  bool quiet = false;
  bool update = false;
  bool all = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintHelp(std::cout);
      return seq::kExitOk;
    }
    if (arg == "--version") {
      PrintVersion(std::cout);
      return seq::kExitOk;
    }
    if (arg == "--settings") {
      PrintSettings(std::cout);
      return seq::kExitOk;
    }
    if (arg == "--set") {
      if (i + 1 >= argc) return Usage("--set needs key=value");
      overrides.emplace_back(argv[++i]);
    } else if (arg.substr(0, 6) == "--set=") {
      overrides.emplace_back(arg.substr(6));
    } else if (arg == "--build-only") {
      build_only = true;
    } else if (arg == "--rebuild") {
      rebuild = true;
    } else if (arg == "--force") {
      force = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--update") {
      update = true;
    } else if (arg == "--all") {
      all = true;
    } else if (arg.size() > 1 && arg.front() == '-') {
      return Usage("unknown option '" + std::string(arg) + "'");
    } else {
      positional.emplace_back(arg);
    }
  }

  if (positional.empty()) {
    PrintHelp(std::cerr);
    return seq::kExitUsage;
  }
  const std::string& command = positional.front();
  const bool build_flags = build_only || rebuild || force || quiet;
  const auto reject_flags = [&](bool allow_update, bool allow_all) -> bool {
    return build_flags || (update && !allow_update) || (all && !allow_all);
  };

  if (command == "new") {
    if (positional.size() != 2) return Usage("usage: seqc new <name>");
    if (reject_flags(false, false)) {
      return Usage("seqc new takes no build options");
    }
    return seq::CommandNew(positional[1], overrides);
  }
  if (command == "check") {
    if (positional.size() != 2) return Usage("usage: seqc check <file.seq>");
    if (reject_flags(false, false)) {
      return Usage("seqc check takes no build options");
    }
    return seq::CommandCheck(positional[1]);
  }
  if (command == "doctor") {
    if (positional.size() != 1 || reject_flags(false, false)) {
      return Usage("usage: seqc doctor");
    }
    return seq::CommandDoctor(overrides);
  }
  if (command == "model") {
    if (positional.size() != 2 || positional[1] != "pull" ||
        reject_flags(true, false)) {
      return Usage("usage: seqc model pull [--update]");
    }
    return seq::CommandModelPull(update, overrides);
  }
  if (command == "clean") {
    if (positional.size() != 1 || reject_flags(false, true)) {
      return Usage("usage: seqc clean [--all]");
    }
    return seq::CommandClean(all);
  }

  if (positional.size() != 1) {
    return Usage("expected one source file, got " +
                 std::to_string(positional.size()) + " arguments");
  }
  if (update || all) return Usage("--update and --all do not apply to a build");
  const bool looks_like_source =
      command.size() > 4 && command.substr(command.size() - 4) == ".seq";
  if (!looks_like_source) {
    return Usage("'" + command + "' is not a command or a .seq source file");
  }
  seq::BuildOptions options;
  options.source = command;
  options.build_only = build_only;
  options.rebuild = rebuild;
  options.force = force;
  options.quiet = quiet;
  options.overrides = overrides;
  return seq::CommandBuild(options);
}

}  // namespace

int main(int argc, char* argv[]) {
  seq::InstallCancellationHandlers();
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "seqc: internal error: " << error.what() << "\n";
    return seq::kExitInternal;
  }
}
