#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "common/util/FileUtil.h"

#include "game/system/IOP_Kernel.h"

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

// jak3 level-heap budget tripwire.
//
// The level heap is carved out of the global heap in one kmalloc. Raising
// DEBUG_LEVEL_HEAP_MULT far enough makes that allocation fail outright, and the failure looks
// nothing like a sizing problem: the level heap never exists, so the boot dies early with an
// unrelated-looking "kmalloc: !alloc mem heap" long before any level loads.
//
// Measured at runtime (session 18): when the level heap is allocated, the global heap is
// 116,114,144 bytes with 63,687,464 already used, leaving 52,426,680 free. 3.00x wants
// 56,512,512 and is REFUSED; 2.75x wants 51,803,136 and fits. Pin the ceiling so a future
// multiplier bump fails here with the arithmetic spelled out instead of at boot.
TEST(TargetArch, Jak3LevelHeapFitsGlobalHeapBudget) {
  const auto mult = read_arm64_heap_mult("jak3");
  if (!mult) {
    return;
  }
  // Mirrors level.gc: LEVEL_PAGE_SIZE = 126 KB, NUM_LEVEL_PAGES = 146.
  constexpr int kLevelPageSize = 126 * 1024;
  constexpr int kNumLevelPages = 146;
  // Measured free space in the global heap at the moment the level heap is kmalloc'd.
  constexpr double kMeasuredFreeBytes = 52426680.0;

  const double page_size = kLevelPageSize * *mult;
  const double heap_size = page_size * kNumLevelPages;

  EXPECT_LE(heap_size, kMeasuredFreeBytes)
      << "jak3 DEBUG_LEVEL_HEAP_MULT = " << *mult << " needs " << static_cast<long>(heap_size)
      << " bytes for the level heap, but only " << static_cast<long>(kMeasuredFreeBytes)
      << " bytes of global heap were free when it is allocated (measured at runtime). The "
         "level-heap kmalloc will fail and the boot will die before any level loads.";
}

// The IOP system clock must WRAP, not saturate (session 20, Bug F).
//
// IOP_Kernel::GetSystemTimeLow reports elapsed time as a u32 count of 36.864 MHz ticks,
// exactly as the PS2's IOP hardware counter does. Its consumers rely on that register
// wrapping: game/overlord/jak3/spustreams.cpp compares timestamps with modular subtraction
// (`(u32)(now - then) < delay`), which is only correct if `now` wraps past 2^32 the way the
// hardware counter does.
//
// The original implementation computed `delta_time.count() * 36.864` -- an int64 times a
// double, yielding a DOUBLE that was then implicitly converted to u32. Converting a double
// whose value is outside u32's range is UNDEFINED BEHAVIOUR, and the two architectures we
// build for disagree about it:
//   x86-64 `cvttsd2si` wraps  -> harmless, which is why this never showed up on x86.
//   ARM64   `fcvtzu`   SATURATES -> every value >= 2^32 becomes 0xFFFFFFFF, permanently.
// 2^32 ticks / 36.864 MHz = 116.5 SECONDS of runtime, after which the clock froze at
// 0xFFFFFFFF on ARM64. Modular subtraction against a frozen clock yields 0 forever, so
// BlockUntilVoiceSafe (spustreams.cpp:1047) spun without yielding, the IOP coroutine never
// returned, the sound RPC never completed, and the game hung. Measured: a jak3 playtest hung
// at 119 s.
//
// The fix does the scaling in the integer domain (36.864 == 4608/125 exactly), which removes
// the UB entirely rather than clamping it, and wraps mod 2^32 on every architecture.
//
// NOTE TO A FUTURE READER: the wraparound below is CORRECT AND REQUIRED -- it is the
// hardware semantic the consumers are written against. Do not "fix" it by widening the
// return type, clamping, or reintroducing floating point.
TEST(TargetArch, IopSystemClockWrapsInsteadOfSaturating) {
  // 2^32 ticks / 36.864 ticks-per-microsecond = the exact wrap point, in microseconds.
  constexpr s64 kWrapMicros = 116508444;  // floor(2^32 / 36.864)

  // Just before the wrap the clock must still be counting normally. 116e6 * 36.864 is
  // exactly 4,276,224,000. Note the old floating-point form returned 4,276,223,999 here:
  // 36.864 is not representable in binary, so the product came out as 4276223999.9999995
  // and truncated one tick low. The integer form is exact, so it is also strictly MORE
  // accurate than the code it replaced -- not merely safer.
  EXPECT_EQ(iop_micros_to_ticks(116000000), 4276224000u)
      << "the IOP clock must still be counting at 116 s (before the 2^32 wrap)";

  // Well past the wrap the value must NOT be pinned at 0xFFFFFFFF. This is the assertion
  // that fails on ARM64 with the original floating-point conversion.
  const u32 at_120s = iop_micros_to_ticks(120000000);
  EXPECT_NE(at_120s, 0xFFFFFFFFu)
      << "the IOP clock SATURATED at 120 s instead of wrapping. On ARM64 an out-of-range "
         "double->u32 conversion saturates (fcvtzu), freezing the clock forever and "
         "deadlocking the overlord's voice spin loops.";

  const u32 at_1000s = iop_micros_to_ticks(1000000000);
  EXPECT_NE(at_1000s, 0xFFFFFFFFu) << "the IOP clock is still pinned at 0xFFFFFFFF at 1000 s";

  // The clock must keep ADVANCING after the wrap, not stick at any single value.
  EXPECT_NE(at_120s, at_1000s)
      << "the IOP clock stopped advancing after wrapping; modular timestamp comparisons in "
         "game/overlord/jak3/spustreams.cpp will never make progress";

  // Exact modular arithmetic: 36.864 == 4608/125, so the tick count is exactly
  // (micros * 4608 / 125) mod 2^32. Check a value chosen to land past the wrap.
  const u64 exact_120s = (120000000ull * 4608ull) / 125ull;
  EXPECT_EQ(at_120s, static_cast<u32>(exact_120s))
      << "the wrapped value must equal the exact integer tick count mod 2^32";

  // And the wrap point itself behaves: one microsecond of extra elapsed time must not
  // produce a smaller-by-more-than-wrap jump.
  EXPECT_EQ(iop_micros_to_ticks(kWrapMicros * 2),
            static_cast<u32>((static_cast<u64>(kWrapMicros) * 2ull * 4608ull) / 125ull))
      << "the clock must wrap cleanly at multiples of the 2^32 boundary";
}

// The ARM64 function pool packs small JIT'd function objects (C trampolines,
// mips2c stubs) into shared executable pages, keyed on the address of the
// heap's kheapinfo. For a level heap that struct is INLINE in the level
// (jak1 level-h.gc: "(heap kheap :inline)"), so the key is a fixed slot reused
// by every level that ever occupies it.
//
// Unloading a level rewinds the heap in GOAL (jak1 level.gc:700,
// "reset the level heap!", current = base) with no C-side notification. If the
// pool keeps handing out addresses from a page carved out of the PREVIOUS
// occupant's heap, the JIT'd code aliases memory the new level has already been
// given for ordinary data.
//
// This models the pool lifecycle across one unload/reload and asserts the pool
// does not hand out an address inside the new level's data. The check under
// test must run on EVERY allocation against the heap: the rewind is only
// observable while the heap is still empty, and a level typically allocates
// ordinary data before its first trampoline.
namespace {

struct FakeHeap {
  u32 base = 0;
  u32 top = 0;
  u32 current = 0;
};

struct FakePool {
  u32 next = 0;
  u32 end = 0;
};

constexpr u32 kFakePageSize = 16384;

// Mirrors arm64_function_pool_note_heap_use() in game/kernel/common/kmalloc.cpp.
void pool_note_heap_use(FakePool* pool, const FakeHeap& heap) {
  if (heap.current > heap.base) {
    return;
  }
  if (pool->next && pool->end > heap.base) {
    pool->next = 0;
    pool->end = 0;
  }
}

// An ordinary (non-pooled) bottom allocation. Goes through the same kmalloc
// entry point, so it observes the heap too.
void fake_data_alloc(FakePool* pool, FakeHeap* heap, u32 size, bool fix_enabled) {
  if (fix_enabled) {
    pool_note_heap_use(pool, *heap);
  }
  heap->current += size;
}

// Mirrors the pooled "function" path of kmalloc().
u32 fake_pool_alloc(FakePool* pool, FakeHeap* heap, u32 size, bool fix_enabled) {
  if (fix_enabled) {
    pool_note_heap_use(pool, *heap);
  }
  const u32 allocation_size = ((size + 0xf) & ~0xfu) + 4;  // object + BASIC_OFFSET
  if (!pool->next || pool->next + allocation_size > pool->end) {
    const u32 page_start = (heap->current + kFakePageSize - 1) & ~(kFakePageSize - 1);
    if (page_start + kFakePageSize > heap->top) {
      return 0;
    }
    heap->current = page_start + kFakePageSize;  // heap bumps past the whole page
    pool->next = page_start;
    pool->end = page_start + kFakePageSize;
  }
  const u32 addr = pool->next;
  pool->next += allocation_size;
  return addr;
}

// Loads a level into a reused heap slot and reports whether a trampoline landed
// inside the new level's ordinary data. `data_first` selects whether the level
// allocates data before or after its first trampoline.
bool trampoline_aliases_level_data(bool fix_enabled, bool data_first) {
  FakeHeap heap;
  heap.base = 0x100000;
  heap.current = heap.base;
  heap.top = heap.base + 18 * 1024 * 1024;
  FakePool pool;

  // First level occupies the slot: two trampolines, then bulk data.
  fake_pool_alloc(&pool, &heap, 0x40, fix_enabled);
  fake_pool_alloc(&pool, &heap, 0x40, fix_enabled);
  fake_data_alloc(&pool, &heap, 4 * 1024 * 1024, fix_enabled);

  // Unload: GOAL rewinds the heap without telling the pool.
  heap.current = heap.base;

  // Second level loads into the same slot.
  u32 data_lo = 0;
  u32 data_hi = 0;
  if (data_first) {
    data_lo = heap.current;
    fake_data_alloc(&pool, &heap, 1024 * 1024, fix_enabled);
    data_hi = heap.current;
  }
  const u32 trampoline = fake_pool_alloc(&pool, &heap, 0x40, fix_enabled);
  if (!data_first) {
    data_lo = heap.current;
    fake_data_alloc(&pool, &heap, 1024 * 1024, fix_enabled);
    data_hi = heap.current;
  }

  return trampoline >= data_lo && trampoline < data_hi;
}

}  // namespace

TEST(TargetArch, Arm64FunctionPoolDroppedWhenLevelHeapIsRewound) {
  // Both allocation orderings must be safe. The data-then-trampoline ordering
  // is the one that defeats every check confined to the pooled allocation path,
  // because by then the heap has been bumped past the stale page again.
  EXPECT_FALSE(trampoline_aliases_level_data(/*fix_enabled=*/true, /*data_first=*/true))
      << "a pooled function allocation was served from the PREVIOUS level's page and landed "
         "inside the new level's data. The staleness check must run on every kmalloc against "
         "the heap, not only on pooled allocations: the rewind is only visible while the heap "
         "is still empty, and a level usually allocates data before its first trampoline.";

  EXPECT_FALSE(trampoline_aliases_level_data(/*fix_enabled=*/true, /*data_first=*/false))
      << "a pooled function allocation aliased the new level's data when the trampoline was "
         "allocated before the level's bulk data";

  // Guard the test itself: without the check, both orderings MUST corrupt.
  // If these ever pass, the model no longer reproduces the bug and the
  // assertions above have stopped proving anything.
  EXPECT_TRUE(trampoline_aliases_level_data(/*fix_enabled=*/false, /*data_first=*/true))
      << "the unfixed model no longer reproduces the aliasing bug; this test has gone vacuous";
  EXPECT_TRUE(trampoline_aliases_level_data(/*fix_enabled=*/false, /*data_first=*/false))
      << "the unfixed model no longer reproduces the aliasing bug; this test has gone vacuous";
}
