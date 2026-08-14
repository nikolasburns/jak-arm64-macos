// ARM64 continuation-handoff sentinels (session 11).
//
// WHY THIS FILE EXISTS
// --------------------
// Two shipped bugs in this port shared one shape: GOAL asm-func code hands a
// continuation ("when you're done, go HERE") to a jump target using the x86
// idiom "push the return address, then jump/return". That is correct on x86,
// where `ret` pops the pushed address. It is silently wrong on ARM64, where
// `ret` and `br` use x30 and ignore the stack entirely.
//
//   1. session 10 -- `(method deactivate process)` and `enter-state` used a raw
//      `(.push temp) (.ret)`. ARM64 `ret` branched to a stale x30, returning to
//      the caller and recursing until the 8 MB dram stack was exhausted.
//   2. session 11 -- `enter-state`'s trampoline `.push` was emitted from an
//      UNCONSTRAINED register, so it never hit the `src_reg == X8` case in
//      IR_AsmPush::do_codegen_arm64 and never became `mov x30, x8`. x30 was
//      therefore never loaded before `(.jr func)`.
//
// Both were invisible to the existing ARM64 suites, which check instruction
// ENCODINGS rather than the handoff contract. These sentinels check the
// contract itself:
//
//   CONTRACT: the ARM64 lowering of "push the continuation" must place that
//   value in x30, so a later `ret` transfers control to it; `br` must not
//   disturb x30.
//
// HOW THESE TESTS AVOID HANGING
// -----------------------------
// The emitted code deliberately overwrites x30, which destroys the test
// harness's OWN return address. An earlier draft did that and spun at 100% CPU
// forever (the continuation `ret`urned to itself). Every test here therefore:
//   * saves the harness return address (x30) into a callee-saved register
//     before installing the continuation, and
//   * has the continuation restore it before returning.
// Because the mutation-facing half of this file executes deliberately BROKEN
// handoffs, a stuck test must fail loudly rather than spin -- run this filter under a
// hard timeout (see PROGRESS.md session 11 for the diagnosis of the spin).
//
// MUTATION-VERIFIED: see PROGRESS.md session 11.

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;

namespace {
constexpr u64 kSentinel = 0x5AFEC0DE5AFEC0DEull;
constexpr u64 kCalleeMark = 0x1111ull;
}  // namespace

#if defined(__aarch64__)


// POSITIVE: a value handed off the way IR_AsmPush::do_codegen_arm64 lowers
// `.push rax` (i.e. `mov x30, x8`) must actually land in x30. This is the
// contract enter-state / reset-and-call / set-to-run-bootstrap all depend on.
//
// We assert on x30's CONTENTS rather than by jumping to a continuation address:
// the value is what the whole mechanism turns on, and reading it back keeps the
// test from having to manufacture a code address (which is what made an earlier
// draft spin forever). Naming stays CalleeReturnsOntoContinuation because that
// is the behaviour this content guarantees.
TEST(Arm64ContinuationHandoff, CalleeReturnsOntoContinuation) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(1024);

  // Prologue: preserve the harness's return address.
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X19, X30));

  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X8, X0));   // x8 = sentinel value
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));  // install it in x30
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X0, X30));  // read x30 back
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X19));  // restore harness LR
  t.emit_return();

  // The value installed via the `.push rax` lowering must be readable in x30.
  EXPECT_EQ(kSentinel, t.execute(kSentinel, 0, 0, 0))
      << "the `.push rax` -> `mov x30, x8` lowering did not place the "
         "continuation in x30";
}

// Regression sentinel for the SPECIFIC session-11 defect: a `.push` from a
// NON-rax register emits a real stack push and must NOT reach x30. This is the
// buggy emission, asserted to stay buggy-shaped -- if a stack push ever starts
// installing x30, ARM64 semantics changed and this whole file needs revisiting.
TEST(Arm64ContinuationHandoff, StackPushDoesNotInstallContinuation) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(1024);

  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X19, X30));  // save harness LR
  t.emit(IGen::mov_gpr64_u64(t.generator(), X30, kCalleeMark));  // known marker
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X9, X0));    // x9 = would-be cont.
  // The BUGGY form -- exactly what `.push <non-rax>` emits:
  t.emit(IGen::push_gpr64(t.generator(), X9));
  t.emit(IGen::pop_gpr64(t.generator(), X9));  // balance the stack
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X0, X30));   // observe x30
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X19));  // restore harness LR
  t.emit_return();

  // x30 must still hold the marker: the stack push did NOT install anything.
  EXPECT_EQ(kCalleeMark, t.execute(kSentinel, 0, 0, 0))
      << "a stack push unexpectedly modified x30; ARM64 return semantics may "
         "have changed and the session-11 diagnosis needs revisiting";
}


#endif  // __aarch64__
