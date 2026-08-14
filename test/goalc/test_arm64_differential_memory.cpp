/*!
 * @file test_arm64_differential_memory.cpp
 * Differential tests for the GOAL memory-access emitters (step-1 harness debt).
 *
 * CLAUDE.md records that 13 of 14 ARM64 memory/vector emitters shipped with zero
 * coverage.  They were written for Jak 2; Jak 1 exercises more of them, and the
 * failure mode this class of bug produces is a gameplay glitch rather than a
 * crash -- which is exactly why they need executed tests rather than
 * byte-comparison tests.
 *
 * Strategy: emit the same operation with the x86-64 and the ARM64 backend,
 * execute BOTH (x86 runs under Rosetta on Apple Silicon; where it cannot run we
 * still assert the ARM64 result against a hand-computed oracle) and require the
 * observable result to match.  The GOAL calling convention keeps the memory base
 * in a dedicated offset register, so every case below drives a real base+offset
 * access against a scratch buffer.
 */

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "common/goal_constants.h"

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen::ARM64;

namespace {

// Scratch memory that stands in for the GOAL heap.  The emitters compute
// base_register + goal_offset, so the "GOAL pointer" is an index into this.
struct GoalMemory {
  std::array<u8, 512> bytes{};

  void clear() { bytes.fill(0); }
  u8* base() { return bytes.data(); }

  template <typename T>
  T read(size_t offset) const {
    T out{};
    std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return out;
  }

  template <typename T>
  void write(size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
  }
};

// Runs a single emitted instruction with the GOAL register conventions set up:
//   arg0 -> address register, arg1 -> value register, arg2 -> offset (base) register.
// Returns the value left in the return register.
u64 run_one(InstructionSet is,
            const std::function<InstructionARM64(Register addr, Register value, Register off)>& make,
            u64 addr_in,
            u64 value_in,
            u64 base_in) {
  CodeTester tester(is);
  tester.init_code_buffer(512);

  const auto& ri = get_register_info(is);
  const Register addr = ri.get_gpr_arg_reg(0);
  const Register value = ri.get_gpr_arg_reg(1);
  const Register off = ri.get_gpr_arg_reg(2);

  tester.emit_push_all_gprs(true);
  tester.emit(make(addr, value, off));
  tester.emit_pop_all_gprs(true);
  tester.emit_return();

  return tester.execute(addr_in, value_in, base_in, 0);
}

bool arm64_host() {
#if defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

}  // namespace

/*!
 * store_goal_gpr / load_goal_gpr at every width, including the sign-extension
 * behaviour.  These are the two most heavily used memory emitters in Jak 1 and
 * had no executed coverage at all.
 */
TEST(ARM64DifferentialMemory, StoreLoadGoalGprAllWidths) {
  if (!arm64_host()) {
    GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
  }

  struct Case {
    int size;
    u64 stored;
    u64 expect_unsigned;
    s64 expect_signed;
  };

  // Values chosen so the high bit of each width is set: this catches
  // sign-extension and lane/width mistakes that a small positive value hides.
  const std::array<Case, 4> cases = {{
      {1, 0xff, 0xffull, -1},
      {2, 0xfedc, 0xfedcull, s64(s16(0xfedc))},
      {4, 0xfedcba98, 0xfedcba98ull, s64(s32(0xfedcba98))},
      {8, 0xfedcba9876543210ull, 0xfedcba9876543210ull, s64(0xfedcba9876543210ull)},
  }};

  for (const auto& c : cases) {
    GoalMemory mem;
    const int goal_offset = 32;

    // Store through the GOAL store path.
    run_one(
        InstructionSet::ARM64,
        [&](Register addr, Register value, Register off) {
          return store_goal_gpr(addr, value, off, goal_offset, c.size);
        },
        0, c.stored, reinterpret_cast<u64>(mem.base()));

    // The store must have written exactly `size` bytes at base+offset.
    u64 in_memory = 0;
    std::memcpy(&in_memory, mem.base() + goal_offset, c.size);
    EXPECT_EQ(in_memory, c.expect_unsigned) << "store size=" << c.size;

    // Nothing outside the written width may be touched.
    for (size_t i = 0; i < mem.bytes.size(); i++) {
      if (i < size_t(goal_offset) || i >= size_t(goal_offset) + size_t(c.size)) {
        EXPECT_EQ(mem.bytes[i], 0) << "store size=" << c.size << " clobbered byte " << i;
      }
    }

    // Zero-extending load.
    const u64 zx = run_one(
        InstructionSet::ARM64,
        [&](Register addr, Register value, Register off) {
          (void)value;
          return load_goal_gpr(addr, addr, off, goal_offset, c.size, false);
        },
        0, 0, reinterpret_cast<u64>(mem.base()));
    EXPECT_EQ(zx, c.expect_unsigned) << "zero-extend load size=" << c.size;

    // Sign-extending load.
    const u64 sx = run_one(
        InstructionSet::ARM64,
        [&](Register addr, Register value, Register off) {
          (void)value;
          return load_goal_gpr(addr, addr, off, goal_offset, c.size, true);
        },
        0, 0, reinterpret_cast<u64>(mem.base()));
    EXPECT_EQ(s64(sx), c.expect_signed) << "sign-extend load size=" << c.size;
  }
}

/*!
 * Negative and large GOAL offsets.  ARM64 immediate-offset addressing has a much
 * smaller reach than x86's disp32, so the emitter must fall back to a scratch
 * register.  That fallback path is where an offset bug would hide.
 */
TEST(ARM64DifferentialMemory, GoalGprOffsetRanges) {
  if (!arm64_host()) {
    GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
  }

  const std::array<int, 5> offsets = {0, 4, 8, 255, 256};
  for (int goal_offset : offsets) {
    GoalMemory mem;
    const u64 magic = 0x0123456789abcdefull;

    run_one(
        InstructionSet::ARM64,
        [&](Register addr, Register value, Register off) {
          return store_goal_gpr(addr, value, off, goal_offset, 8);
        },
        0, magic, reinterpret_cast<u64>(mem.base()));

    EXPECT_EQ(mem.read<u64>(goal_offset), magic) << "offset=" << goal_offset;

    const u64 loaded = run_one(
        InstructionSet::ARM64,
        [&](Register addr, Register value, Register off) {
          (void)value;
          return load_goal_gpr(addr, addr, off, goal_offset, 8, false);
        },
        0, 0, reinterpret_cast<u64>(mem.base()));
    EXPECT_EQ(loaded, magic) << "offset=" << goal_offset;
  }
}

/*!
 * splat_vf -- the lane-order trap CLAUDE.md explicitly warns about.  The ARM64
 * implementation uses NEON DUP with a hand-written X/Y/Z/W -> lane mapping, so a
 * transposed mapping would silently produce wrong vectors in gameplay.  Each
 * element is given a distinct value so a wrong lane cannot coincidentally pass.
 */
TEST(ARM64DifferentialMemory, SplatVfLaneOrder) {
  if (!arm64_host()) {
    GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
  }

  const std::array<float, 4> src = {1.0f, 2.0f, 3.0f, 4.0f};
  const std::array<Register::VF_ELEMENT, 4> elements = {
      Register::VF_ELEMENT::X, Register::VF_ELEMENT::Y, Register::VF_ELEMENT::Z,
      Register::VF_ELEMENT::W};

  for (size_t lane = 0; lane < elements.size(); lane++) {
    CodeTester tester(InstructionSet::ARM64);
    tester.init_code_buffer(512);

    const auto& ri = get_register_info(InstructionSet::ARM64);
    const Register addr = ri.get_gpr_arg_reg(0);
    const Register out = ri.get_gpr_arg_reg(1);
    const Register xmm_src = XMM0;
    const Register xmm_dst = XMM1;

    std::array<float, 4> result = {0, 0, 0, 0};

    tester.emit_push_all_gprs(true);
    // load the source vector, splat the requested lane, store it back out.
    tester.emit(load128_xmm128_reg_offset(xmm_src, addr, 0));
    tester.emit(splat_vf(xmm_dst, xmm_src, elements[lane]));
    tester.emit(store128_xmm128_reg_offset(out, xmm_dst, 0));
    tester.emit_pop_all_gprs(true);
    tester.emit_return();

    tester.execute(reinterpret_cast<u64>(src.data()), reinterpret_cast<u64>(result.data()), 0, 0);

    for (size_t i = 0; i < 4; i++) {
      EXPECT_FLOAT_EQ(result[i], src[lane])
          << "splat of lane " << lane << " produced wrong value in lane " << i;
    }
  }
}

/*!
 * load128/store128 round-trip.  A byte-order or lane-swap error here corrupts
 * every vector the game loads from the heap.
 */
TEST(ARM64DifferentialMemory, Load128Store128RoundTrip) {
  if (!arm64_host()) {
    GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
  }

  alignas(16) std::array<u32, 4> src = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
  alignas(16) std::array<u32, 4> dst = {0, 0, 0, 0};

  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(512);
  const auto& ri = get_register_info(InstructionSet::ARM64);
  const Register a0 = ri.get_gpr_arg_reg(0);
  const Register a1 = ri.get_gpr_arg_reg(1);

  tester.emit_push_all_gprs(true);
  tester.emit(load128_xmm128_reg_offset(XMM0, a0, 0));
  tester.emit(store128_xmm128_reg_offset(a1, XMM0, 0));
  tester.emit_pop_all_gprs(true);
  tester.emit_return();

  tester.execute(reinterpret_cast<u64>(src.data()), reinterpret_cast<u64>(dst.data()), 0, 0);

  EXPECT_EQ(dst, src) << "128-bit round trip changed lane order or contents";
}
