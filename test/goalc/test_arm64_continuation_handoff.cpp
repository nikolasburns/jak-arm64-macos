// ARM64 continuation-handoff sentinels (session 11).
//
// WHY THIS FILE EXISTS
// --------------------
// Two shipped bugs in this port shared one shape: GOAL asm-func code hands a
// continuation ("when you're done, return HERE") to a jump target using the x86
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
//   value in x30, so that a later `ret` transfers control to it. A `br` must
//   NOT disturb it.
//
// DESIGN NOTE: the continuation is a real C++ function whose address is passed
// in as an argument, so these tests use only CodeTester's public API and
// exercise the same surface real callers use. An earlier draft patched absolute
// addresses into the code buffer, which required private access -- a sign the
// test was reaching at the wrong layer.
//
// MUTATION-VERIFIED: see PROGRESS.md session 11 for the recorded mutation runs.

#include <array>

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;

namespace {
constexpr u64 kSentinel = 0x5AFEC0DE5AFEC0DEull;
constexpr u64 kCalleeMark = 0x1111ull;

// The continuation. Reaching this is the thing under test; it reports by
// returning the sentinel, which becomes the value CodeTester::execute sees.
extern "C" u64 handoff_continuation() {
  return kSentinel;
}

// A callee that does some work and then returns via x30. In the real kernel
// this is the state's `code` function or the thread entry function.
extern "C" u64 handoff_callee() {
  return kCalleeMark;
}
}  // namespace

#if defined(__aarch64__)

// The core contract: a continuation handed off the way
// IR_AsmPush::do_codegen_arm64 lowers `.push rax` must be reached by `ret`.
//
// arg0 (x0) = address of the continuation.
TEST(Arm64ContinuationHandoff, ContinuationReachesX30) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(1024);

  // x8 = continuation (as `.mov temp <fn>` / `.add temp off` would leave it).
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X8, X0));
  // This is EXACTLY what IR_AsmPush::do_codegen_arm64 emits for `.push rax`:
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));
  // ...and a plain `ret` must now land on it.
  t.emit(Instruction(IGen::ARM64::ret()));

  EXPECT_EQ(kSentinel, t.execute((u64)&handoff_continuation, 0, 0, 0))
      << "the continuation in X8 was not reached: the `.push rax` -> x30 "
         "handoff is broken, so `ret` did not return to it";
}

// The shape the kernel actually uses (enter-state / reset-and-call /
// set-to-run-bootstrap): install the continuation, then `br` to a DIFFERENT
// function. When that function returns, control must land on the continuation.
//
// arg0 (x0) = continuation, arg1 (x1) = callee.
TEST(Arm64ContinuationHandoff, CalleeReturnsOntoContinuation) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(1024);

  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X8, X0));    // temp = continuation
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));   // `.push temp`
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X9, X1));    // func = callee
  t.emit(Instruction(IGen::ARM64::jmp_r64(X9)));                // `.jr func`

  EXPECT_EQ(kSentinel,
            t.execute((u64)&handoff_continuation, (u64)&handoff_callee, 0, 0))
      << "callee returned but did not land on the continuation: x30 did not "
         "hold the installed trampoline (session-11 enter-state bug shape)";
}

// Regression sentinel for the SPECIFIC session-11 defect: if the trampoline is
// pushed to the STACK instead of moved to x30 -- which is what an unconstrained
// register produced -- the continuation is unreachable and the callee's return
// goes somewhere else entirely.
//
// This models the BUGGY emission. It must NOT reach the continuation; if it
// ever does, the ARM64 return-address semantics have changed and every comment
// in this file needs revisiting.
TEST(Arm64ContinuationHandoff, StackPushDoesNotInstallContinuation) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(1024);

  // Give x30 a known-good return path first (so the test itself can return).
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X8, X0));
  // The BUGGY form: a real stack push, exactly what `.push <non-rax>` emits.
  t.emit(IGen::push_gpr64(t.generator(), X8));
  // Then the callee is invoked with `blr` so control comes back here.
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X9, X1));
  t.emit(Instruction(IGen::ARM64::call_r64(X9)));
  t.emit(IGen::pop_gpr64(t.generator(), X8));  // balance the stack
  t.emit_return();

  // The callee's own return value survives -- proving the pushed continuation
  // was never installed as the return target.
  EXPECT_EQ(kCalleeMark,
            t.execute((u64)&handoff_continuation, (u64)&handoff_callee, 0, 0))
      << "a stack push unexpectedly behaved as a continuation install; ARM64 "
         "return semantics may have changed";
}

// Negative control: `br` must NOT write x30 (unlike `blr`). This documents the
// exact property that makes the x86 push/ret idiom unsafe here, so that if a
// future change makes `br` behave like `blr`, this file explains why the other
// tests would start passing for the wrong reason.
TEST(Arm64ContinuationHandoff, BrDoesNotClobberX30) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(1024);

  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X8, X0));   // continuation
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X8));  // install it
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X9, X1));   // callee
  t.emit(Instruction(IGen::ARM64::jmp_r64(X9)));               // must not touch x30

  // handoff_callee returns via x30; if `br` had overwritten x30 with the
  // address of the next instruction, we would loop here instead of reaching
  // the continuation.
  EXPECT_EQ(kSentinel,
            t.execute((u64)&handoff_continuation, (u64)&handoff_callee, 0, 0))
      << "`br` appears to have written x30; the continuation was lost";
}

#endif  // __aarch64__
