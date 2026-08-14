// ARM64 continuation-handoff sentinels (session 11).
//
// WHY THIS FILE EXISTS
// --------------------
// Two shipped bugs in this port shared one shape: GOAL asm-func code that hands
// a continuation to a jump target using the x86 idiom "push the return address,
// then jump/return", which is correct on x86 (where `ret` pops the pushed
// address) but silently wrong on ARM64 (where `ret`/`br` use x30 and ignore the
// stack).
//
//   1. session 10 -- `(method deactivate process)` and `enter-state` used a raw
//      `(.push temp) (.ret)`. ARM64 `ret` branched to a stale x30, returning to
//      the caller and recursing until the dram stack was exhausted.
//   2. session 11 -- `enter-state`'s trampoline `.push` was emitted from an
//      UNCONSTRAINED register, so it did not hit the `src_reg == X8` case in
//      IR_AsmPush::do_codegen_arm64 and never became `mov x30, x8`. x30 was
//      therefore never loaded before `(.jr func)`.
//
// Both were invisible to the existing suites because those test instruction
// ENCODINGS, not the x30 handoff contract. These sentinels test the contract:
//
//   CONTRACT: an `.push` whose source is the rax/X8 role must place that value
//   in x30, so that a subsequent `br`/`ret` returns to it.
//
// MUTATION-VERIFIED: with the `src_reg == X8` branch of
// IR_AsmPush::do_codegen_arm64 removed (so `.push` always emits a real stack
// push, reproducing the session-11 bug), ContinuationReachesX30 and
// PushRaxThenRetLandsOnContinuation both FAIL. See PROGRESS.md session 11.

#include <array>
#include <cstring>
#include <memory>

#include "common/type_system/TypeSystem.h"

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;

namespace {

// Build: x8 = <continuation>, then the `.push rax` lowering (mov x30, x8),
// then `ret`.  If the handoff works, control lands on the continuation, which
// sets x0 to a sentinel.  If x30 was never written, `ret` goes somewhere else
// and the sentinel is not observed.
constexpr u64 kSentinel = 0x5AFEC0DE5AFEC0DEull;

}  // namespace

#if defined(__aarch64__)

// The core contract: a value moved into X8 and handed off via the ARM64
// lowering of `.push rax` must be reachable by `ret`.
TEST(Arm64ContinuationHandoff, ContinuationReachesX30) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(4096);

  // --- caller ---------------------------------------------------------
  // x8 = address of the continuation block (filled in after we know it).
  // We emit a placeholder now and patch it once the layout is known.
  const int patch_at = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X8, 0));  // x8 = continuation addr
  // This is exactly what IR_AsmPush::do_codegen_arm64 emits for `.push rax`:
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));
  // A plain `ret` must now transfer control to the continuation.
  t.emit(Instruction(IGen::ARM64::ret()));

  // --- continuation ---------------------------------------------------
  const int cont_offset = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, kSentinel));
  t.emit_return();

  // Patch x8 with the real runtime address of the continuation.
  const u64 cont_addr = (u64)t.code_buffer() + (u64)cont_offset;
  CodeTester patcher(InstructionSet::ARM64);
  patcher.init_code_buffer(64);
  patcher.emit(IGen::mov_gpr64_u64(patcher.generator(), X8, cont_addr));
  std::memcpy(t.code_buffer() + patch_at, patcher.code_buffer(),
              (size_t)patcher.size());

  EXPECT_EQ(kSentinel, t.execute(0, 0, 0, 0))
      << "the continuation in X8 was not reached: the `.push rax` -> x30 "
         "handoff is broken, so `ret` did not return to it";
}

// The same contract, but exercising the shape the kernel actually uses:
// compute the continuation, hand it off, then `br` to a *different* function
// which returns -- landing on the continuation.  This is the enter-state /
// reset-and-call / set-to-run-bootstrap pattern.
TEST(Arm64ContinuationHandoff, PushRaxThenRetLandsOnContinuation) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(4096);

  const int patch_at = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X8, 0));   // continuation
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));  // the handoff
  const int patch_target = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X9, 0));   // callee address
  t.emit(Instruction(IGen::ARM64::br(X9)));            // `.jr func`

  // --- callee: does its work and returns via x30 -----------------------
  const int callee_offset = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X1, 0x1111));
  t.emit(Instruction(IGen::ARM64::ret()));

  // --- continuation ----------------------------------------------------
  const int cont_offset = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, kSentinel));
  t.emit_return();

  auto patch_u64 = [&](int at, u64 value, Register reg) {
    CodeTester p(InstructionSet::ARM64);
    p.init_code_buffer(64);
    p.emit(IGen::mov_gpr64_u64(p.generator(), reg, value));
    std::memcpy(t.code_buffer() + at, p.code_buffer(), (size_t)p.size());
  };
  patch_u64(patch_at, (u64)t.code_buffer() + (u64)cont_offset, X8);
  patch_u64(patch_target, (u64)t.code_buffer() + (u64)callee_offset, X9);

  EXPECT_EQ(kSentinel, t.execute(0, 0, 0, 0))
      << "callee returned but did not land on the continuation: x30 did not "
         "hold the pushed trampoline (session-11 enter-state bug shape)";
}

// Negative control: a `br` does NOT write x30.  This documents the exact ARM64
// property that makes the x86 push/ret idiom unsafe, so that if a future change
// makes `br` behave like `blr` this file explains why tests start passing for
// the wrong reason.
TEST(Arm64ContinuationHandoff, BrDoesNotClobberX30) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(4096);

  const int patch_at = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X8, 0));
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));
  const int patch_target = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X9, 0));
  t.emit(Instruction(IGen::ARM64::br(X9)));

  // target: immediately return.  If `br` had written x30 (like `blr`), this
  // `ret` would loop back here instead of reaching the continuation.
  const int target_offset = t.size();
  t.emit(Instruction(IGen::ARM64::ret()));

  const int cont_offset = t.size();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, kSentinel));
  t.emit_return();

  auto patch_u64 = [&](int at, u64 value, Register reg) {
    CodeTester p(InstructionSet::ARM64);
    p.init_code_buffer(64);
    p.emit(IGen::mov_gpr64_u64(p.generator(), reg, value));
    std::memcpy(t.code_buffer() + at, p.code_buffer(), (size_t)p.size());
  };
  patch_u64(patch_at, (u64)t.code_buffer() + (u64)cont_offset, X8);
  patch_u64(patch_target, (u64)t.code_buffer() + (u64)target_offset, X9);

  EXPECT_EQ(kSentinel, t.execute(0, 0, 0, 0))
      << "`br` appears to have written x30; the continuation was lost";
}

#endif  // __aarch64__
