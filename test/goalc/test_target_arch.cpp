#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "common/util/FileUtil.h"

#include "goalc/emitter/InstructionSet.h"
#include "goalc/make/MakeSystem.h"

#include "fmt/format.h"
#include "fmt/ranges.h"
#include "gtest/gtest.h"

namespace {

std::string write_test_project(const std::string& prefix) {
  const auto path = file_util::get_file_path({"test", "target_arch_tmp.gp"});
  std::ofstream out(path, std::ios::trunc);
  out << "(set-output-prefix \"" << prefix << "\")\n";
  out.close();
  return path;
}

std::string write_test_project_with_out_mapping() {
  const auto path = file_util::get_file_path({"test", "target_arch_tmp.gp"});
  std::ofstream out(path, std::ios::trunc);
  out << "(map-path! \"$OUT\" \"out/targetarch/\")\n";
  out << "(set-output-prefix \"targetarch/\")\n";
  out << "(defstep :in \"test/target_arch_input\" :tool 'copy :out '(\"$OUT/probe\"))\n";
  out.close();
  return path;
}

}  // namespace

TEST(InstructionSet, ToString) {
  EXPECT_STREQ(emitter::to_string(emitter::InstructionSet::X86), "x86_64");
  EXPECT_STREQ(emitter::to_string(emitter::InstructionSet::ARM64), "arm64");
}

TEST(InstructionSet, ParseValid) {
  EXPECT_EQ(emitter::parse_instruction_set("x86_64"), emitter::InstructionSet::X86);
  EXPECT_EQ(emitter::parse_instruction_set("arm64"), emitter::InstructionSet::ARM64);
}

TEST(InstructionSet, ParseInvalidThrows) {
  EXPECT_THROW(emitter::parse_instruction_set(""), std::runtime_error);
  EXPECT_THROW(emitter::parse_instruction_set("x86"), std::runtime_error);
  EXPECT_THROW(emitter::parse_instruction_set("arm"), std::runtime_error);
  EXPECT_THROW(emitter::parse_instruction_set("riscv64"), std::runtime_error);
}

TEST(InstructionSet, OutputSuffix) {
  EXPECT_STREQ(emitter::output_suffix(emitter::InstructionSet::X86), "");
  EXPECT_STREQ(emitter::output_suffix(emitter::InstructionSet::ARM64), "-arm64");
}

TEST(TargetArch, X86OutputPrefixUnchanged) {
  const auto path = write_test_project("targetarch/");
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::X86);
  make.load_project_file(path);
  EXPECT_EQ(make.compiler_output_prefix(), "targetarch/");
  fs::remove(path);
}

TEST(TargetArch, Arm64OutputPrefixSeparated) {
  const auto path = write_test_project("targetarch/");
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::ARM64);
  make.load_project_file(path);
  EXPECT_EQ(make.compiler_output_prefix(), "targetarch-arm64/");
  fs::remove(path);
}

TEST(TargetArch, Arm64RemapsProjectOutWithPrefix) {
  const auto path = write_test_project_with_out_mapping();
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::ARM64);
  make.load_project_file(path);
  EXPECT_EQ(make.get_dependencies("out/targetarch-arm64/probe").size(), 1);
  fs::remove(path);
}

TEST(TargetArch, X86ProjectOutMappingUnchanged) {
  const auto path = write_test_project_with_out_mapping();
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::X86);
  make.load_project_file(path);
  EXPECT_EQ(make.get_dependencies("out/targetarch/probe").size(), 1);
  fs::remove(path);
}

// The tests above pin the COMPILER side: goalc WRITES ARM64 output to out/<game>-arm64/.
// The two below pin the RUNTIME side, a separate failure mode: gk must READ from that same tree.
// jak3's first boot attempt SIGILL'd because CISOCDFileSystem::ReadDirectory built its path from a
// hardcoded "out/jak3/iso" literal instead of get_game_output_dir(), so an ARM64 gk was served
// x86_64 KERNEL.CGO and link-and-exec jumped straight into x86 opcodes.

TEST(TargetArch, GameOutputDirIsArchSeparated) {
  for (auto game : {GameVersion::Jak1, GameVersion::Jak2, GameVersion::Jak3}) {
    const auto leaf = file_util::get_game_output_dir(game).filename().string();
    const bool has_suffix = leaf.size() > 6 && leaf.compare(leaf.size() - 6, 6, "-arm64") == 0;
#if defined(__APPLE__) && defined(__aarch64__)
    EXPECT_TRUE(has_suffix) << "runtime output dir must be arch-separated on ARM64 macOS: " << leaf;
#else
    EXPECT_FALSE(has_suffix) << "non-ARM64 builds must not use the -arm64 tree: " << leaf;
#endif
  }
}

// Source-level tripwire. A hardcoded "out/jakN/" literal in the runtime compiles and links
// perfectly and only fails at boot, with a SIGILL several layers removed from the typo. Grep for
// the literal so the mistake is caught at test time instead of costing a boot cycle.
TEST(TargetArch, RuntimeHasNoHardcodedGameOutputPaths) {
  const std::regex bad_path(R"(out/jak[123]/(obj|iso)|"out"[ \t]*/[ \t]*"jak[123]")");

  std::vector<std::string> offenders;
  for (const auto& dir : {"game", "common"}) {
    const auto root = file_util::get_jak_project_dir() / dir;
    if (!fs::exists(root)) {
      continue;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      const auto ext = entry.path().extension().string();
      if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
        continue;
      }
      std::ifstream in(entry.path());
      std::string line;
      int line_no = 0;
      while (std::getline(in, line)) {
        line_no++;
        if (std::regex_search(line, bad_path)) {
          offenders.push_back(fmt::format("{}:{}", entry.path().string(), line_no));
        }
      }
    }
  }

  EXPECT_TRUE(offenders.empty())
      << "Runtime code must build game output paths via file_util::get_game_output_dir(), never a "
         "hardcoded out/jakN literal -- an ARM64 build would read the x86 tree and SIGILL at "
         "link-and-exec. Offending lines:\n"
      << fmt::format("{}", fmt::join(offenders, "\n"));
}

namespace {
// Read the ARM64 branch of DEBUG_LEVEL_HEAP_MULT out of a game's level.gc, e.g.
//   (defglobalconstant DEBUG_LEVEL_HEAP_MULT (#if ARM64_TARGET 2.25 1.2))
std::optional<double> read_arm64_heap_mult(const std::string& game) {
  const auto path =
      file_util::get_jak_project_dir() / "goal_src" / game / "engine" / "level" / "level.gc";
  if (!fs::exists(path)) {
    return std::nullopt;
  }
  std::ifstream in(path);
  std::string line;
  const std::regex re(R"(DEBUG_LEVEL_HEAP_MULT\s*\(#if\s+ARM64_TARGET\s+([0-9.]+))");
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, re)) {
      return std::stod(m[1].str());
    }
  }
  return std::nullopt;
}
}  // namespace

// Level-heap sizing tripwire.
//
// ARM64 objects are much larger than their x86 counterparts, so each level's heap has to be
// scaled by DEBUG_LEVEL_HEAP_MULT. When that multiplier is too small the failure is a mid-boot
// "dgo file header ... has overrun heap ... by N bytes" -- which costs a whole boot cycle to find
// and names the object that happened to cross the line rather than the constant that is wrong.
//
// jak3 shipped 1.75x (copied from jak1/jak2) and WASALL.DGO overran it by 1632 bytes: the
// multiplier had been justified against the LARGEST DGO, but the largest DGOs are art-dominated
// (VOCA is 13.8 MB at ratio 1.14) and scale for free. The ones that overrun are mid-size and
// code-dense. So compare the multiplier against the measured per-DGO growth ratio, and report the
// DGOs that exceed it.
//
// This is advisory, not a hard bound: a DGO above the multiplier only overruns if its own page
// allocation is too tight, so exceeding it is a "look at this" and not a proven failure. The test
// fails only if a game's ARM64 tree is present AND the WORST ratio is above the multiplier by a
// wide margin, which is the state that produced the jak3 bug.
TEST(TargetArch, LevelHeapMultiplierCoversMeasuredDgoGrowth) {
  for (const auto& game : {"jak1", "jak2", "jak3"}) {
    const auto mult = read_arm64_heap_mult(game);
    if (!mult) {
      continue;  // game uses a different sizing scheme (jak1 sets LEVEL_HEAP_SIZE directly)
    }
    const auto arm_dir = file_util::get_jak_project_dir() / "out" / (std::string(game) + "-arm64") /
                         "iso";
    const auto x86_dir = file_util::get_jak_project_dir() / "out" / game / "iso";
    if (!fs::exists(arm_dir) || !fs::exists(x86_dir)) {
      continue;  // both trees are needed to measure; skip rather than fail on a partial checkout
    }

    std::vector<std::pair<double, std::string>> over;
    double worst = 0.0;
    for (const auto& entry : fs::directory_iterator(arm_dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".DGO") {
        continue;
      }
      const auto x86_file = x86_dir / entry.path().filename();
      if (!fs::exists(x86_file)) {
        continue;
      }
      const auto x86_size = fs::file_size(x86_file);
      if (x86_size == 0) {
        continue;
      }
      const double ratio = static_cast<double>(fs::file_size(entry.path())) /
                           static_cast<double>(x86_size);
      worst = std::max(worst, ratio);
      if (ratio > *mult) {
        over.emplace_back(ratio, entry.path().filename().string());
      }
    }
    if (over.empty()) {
      continue;
    }
    std::sort(over.rbegin(), over.rend());

    // A single DGO 1.5x beyond the configured multiplier is the shape that actually overran.
    EXPECT_LT(worst, *mult * 1.5)
        << game << ": DEBUG_LEVEL_HEAP_MULT is " << *mult
        << " on ARM64, but the worst measured level-DGO growth is " << worst
        << "x. That is the configuration that produced the WASALL.DGO overrun. Raise the "
           "multiplier or confirm these levels' page allocations are large enough:\n"
        << fmt::format("{}", fmt::join(over, "\n"));
  }
}
