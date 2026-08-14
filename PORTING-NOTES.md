# Porting OpenGOAL to ARM64: engineering notes

Practical notes from porting Jak 1 to native ARM64 macOS, on top of the ARM64
backend that [DiMiTriFrog/jak2-macos-arm64](https://github.com/DiMiTriFrog/jak2-macos-arm64)
built for Jak 2.

**Who this is for:** anyone extending OpenGOAL to a new architecture, or adding
Jak 3 support to this backend. Most of what follows is not Jak-1-specific — it is
about what breaks when GOAL's x86-shaped assumptions meet a RISC target, and
which debugging techniques actually pay off against a JIT'd Lisp runtime.

Everything here was learned by getting it wrong first. Dead ends are included on
purpose: knowing which five hypotheses were wrong is often worth more than the
one that was right.

---

## 1. The dominant bug class: "x86 semantics in ARM64 clothing"

**Definition:** GOAL asm-form code that is correct only because of an x86
instruction's *side effects*, compiled unchanged for ARM64.

Seven instances were found and fixed. Every one was a place where Jak 2 had
already been ported correctly and Jak 1's mirror was incomplete:

| # | Site | The x86 assumption |
|---|---|---|
| 1 | C→GOAL bridges (`asm_funcs_arm64.s`) | saved-register *role names* |
| 2 | `gkernel` context-switch routines | same role names |
| 3 | catch/throw saved set | same role names |
| 4 | `cpu-thread` backup stacks | x86-sized frames |
| 5 | `deactivate` + `enter-state` | `ret` consumes a pushed address |
| 6 | `enter-state` trampoline | `.push` reaches x30 only from the rax role |
| 7 | level-heap sizing | heap sized to x86 code density |

### The tell

Any `rlet` / `asm-func` / `.form` construct whose correctness depends on an x86
instruction's **implicit operand** — the register it names, the stack it touches,
or the size it assumes.

Two concrete traps that cost the most time:

**ARM64 has SEVEN allocator-saved GPRs — `{X19, X23..X28}` — but GOAL's historical
x86 vocabulary exposes only five.** Any code naming saved registers as
`rbx/rbp/r10/r11/r12` covers only X19 and X23 on ARM64; `rbp/r10/r11` map to
X29/X6/X7, which are *not* allocator-saved. Every such site must use
`saved-gpr0..4` and be wrapped in `with-context-extra-saved-gprs`.

**`ret` and `br` use X30 and ignore the stack.** The x86 idiom "push the return
address, then `ret`" silently does nothing useful. On ARM64 the continuation must
be *moved into X30*. In this codebase that lowering exists
(`IR_AsmPush::do_codegen_arm64`) but fires **only** when the push source is X8
(the historical `rax` role) — so an unconstrained `temp` produces a real stack
push and the trampoline is lost. That was bug #6, and it presented as a null jump
several minutes and one level-load away from its cause.

### The method that actually finds these

**Three-way static diff: fork-jakN vs fork-jakM vs upstream-jakN**, of the
suspect routine.

This cost ~10 minutes and solved a blocker that three sessions of runtime
instrumentation could only photograph. Prefer it over *any* runtime probe.

Corollary, learned the hard way: a hardware watchpoint once captured the
corrupting instruction perfectly and still did not yield the cause, because it
found the **writer** rather than the **control-flow error that made the writer
run forever**. When a capture explains the mechanism but not the reason, switch
to static diff rather than tightening the instrument.

**Important limit:** the three-way diff applies to shared *kernel contracts*.
Per-game *engine* files legitimately diverge — Jak 2's `auto-save` has different
fields and a different `:state-methods` order than Jak 1's, so "porting Jak 2's
version" there would have changed method-slot numbering and manufactured a
null-slot bug. Divergence is not automatically a defect.

### Sweeps that must come back clean

```sh
grep -rn ":reg rbx\|:reg rbp\|:reg r10 \|:reg r11 \|:reg r12 " goal_src/
grep -rn "(\.ret)" goal_src/jak1/   # each hit must be #if-guarded, or a
                                    # deliberate pop/push address rewrite
```

A regression suite for this class lives in
`test/goalc/test_arm64_continuation_handoff.cpp`. It drives `IR_AsmPush`
directly and asserts on **emitted bytes**, with a table row per real call site.
See §5 for why that layer matters.

---

## 2. Code density: the constant nobody thinks to resize

ARM64 code objects are **2.5–3.2× larger** than x86 for the same GOAL source
(measured: `target.o` 2.53×, `level.o` 2.96×, `gkernel.o` 2.69×). Fixed-width
4-byte instructions plus multi-instruction address computation where x86 uses one
addressed load.

Whole DGOs grow far less — **1.01–1.13×** — because they are dominated by
arch-neutral art and geometry. But that is still enough to exhaust a heap sized
for x86 output:

```
ERROR: dgo file header ... has overrun heap ... by 267056 bytes. This is very bad!
dkernel: heap overflow
```

Jak 2's port had already raised its multiplier (`(#if ARM64_TARGET 1.75 1.1)`);
Jak 1's mirror was stock, so the largest levels failed to load. **If you port to
a new architecture, audit every heap/buffer constant sized against code output.**

A subtlety worth knowing: a DGO larger than the level heap is *normal* and
happens on x86 too. The runtime check compares **one object's end** against
`top-base`, not the whole file — so the failure is size-dependent and surfaces at
whichever level first crosses the line, not at the first oversized DGO.

---

## 3. Debugging a JIT'd Lisp runtime

### GOAL null-deref does not fault

`(-> null-obj slot)` does **not** trap. A null/`#f` object plus a small field
offset lands in mapped low memory near the symbol table, so the read *succeeds*
and returns whatever is there — usually 0.

Consequences:

- **The faulting instruction is not where the bug is.** A `br`/`blr` to 0 is the
  first thing that traps; the null entered the data one or more steps earlier.
- **Zero recoverable frames is expected**, not anomalous: a `blr` to 0 writes a
  null X30, so there is no caller chain. The caller's identity survives only in
  whatever it left in other registers.
- Never hunt for "the writer of 0" at the fault address. Walk backward from the
  register fingerprint to the fetch, then to whatever left that field null.

### Register archaeology beats stack traces

With no frames, cross-run register comparison is the tool. Registers that are
**identical every run** are link-time addresses (resolve them against the load
map); registers that **move but keep a constant delta** are heap pointer pairs.
That distinction repeatedly narrowed the search faster than any breakpoint.

### lldb watchpoints work — with the right recipe

Hardware watchpoints do work against GOAL code. Earlier failures were an lldb
*syntax* problem, not a platform limit:

1. Arm from a **C anchor function** at the moment the target address is known
   valid, not at a guessed wall-clock moment.
2. Use a **python callback in a script file** (`breakpoint command add -F mod.fn`).
   The inline `-s python -o "..."` form fails with `error: No input data`.
3. **Do not read GOAL context registers (x21/x22) at a C breakpoint** — they only
   mean anything inside GOAL code. Take the GOAL base from the runtime's printed
   constant and arg0 via `frame.args`.
4. **Verify the target is readable and holds the expected value before arming.**
   lldb will happily watch an unmapped address and report success. A successful
   arm is not a correct arm — this produced one entirely wasted capture.

Apple Silicon gives 4 hardware watchpoints.

### Disassembling a linked `.o`

Two traps make naive attempts produce plausible garbage:

- `code_infos[].offset` is relative to `m_object_data`, not the file start.
- The real code start is **not 4-byte aligned in the file**. Find the phase by
  maximising the count of 4-byte-aligned `0xd503201f` (nop); the wrong phase
  emits SME instructions and `<bad>`.
- **The phase changes on every rebuild.** Adding instrumentation moved one object
  from phase 1 to phase 2; reusing the stale phase reported *zero* `br`
  instructions in a segment containing seven. Re-derive every time, and validate
  against an instruction-shape invariant (e.g. "N `.jr` forms in source must
  yield N `br` instructions").

Also: a field offset and a method slot can be the same number. Distinguish them
by whether a `-4` type-word load precedes the offset add.

### Instruments that cannot fail are worthless

Several hours went into instruments that could not have detected the thing they
were pointed at:

- A watchpoint armed at the wrong address — silence proved nothing.
- A test-runner that printed `EXITED code=0 <-- CRASH` for a clean user quit.
  The correct reading: exit 0 **plus** `GOAL Runtime Shutdown` in the log is a
  clean shutdown, never a crash. (139 = SIGSEGV, 138 = SIGBUS, 132 = SIGILL
  = wrong-arch objects, 134 = missing files.)
- Guards placed at the *jump* in a bug class where the null arrives earlier.

**Validate every instrument on known-good input before trusting its silence.**
Make it report once in the healthy case; a guard that has never produced output
proves nothing when quiet.

---

## 4. Verifying against the running game

- **The runtime loads code from `out/<game>/iso/*.DGO|CGO`, not `obj/*.o`.**
  Recompiling a single file in the REPL updates `obj/` only. Always re-run `(mi)`
  before drawing conclusions from runtime behaviour.
- **Drive the compiler with `--cmd`, never `echo "(mi)" | goalc`.** Piping wedges
  the REPL at 99% CPU *after a successful build*, which looks exactly like a hung
  build.
- **"It crashed after doing X" is not "it crashed at X".** One investigation
  spent a whole cycle on a stall warning that the game had survived — the actual
  death was 76 log lines and one level-load later. Locate the death point by line
  number relative to the suspect event before accepting a trigger attribution.
- Missing-asset warnings can be *upstream-normal*. Jak 1 requests
  `sage-intro-sequence-b`, which shipped on no US disc and appears in no object
  table; the loader's handling is a designed non-fatal path. Check the disc
  contents and the object tables before treating a "missing file" as a defect.

---

## 5. Testing at the layer the bug lives in

The most useful lesson of the whole effort.

A regression test was written for the `.push`→x30 lowering, and it passed. Then
the fix was deliberately reverted to check the test would catch it — and **it
still passed**. The test hand-emitted `mov x30, x8` and therefore verified *ARM64
hardware semantics* (never in doubt) rather than *the compiler's codegen
decision*. Removing the lowering could not affect a test that never invoked it.

The rewrite drives `IR_AsmPush::do_codegen_arm64` and asserts on emitted bytes.
Same mutation now fails it, naming the defect exactly:

```
IR_AsmPush with an X8 source did not emit `mov x30, x8` (got 0xf81f0fe8)
```

`0xf81f0fe8` is `str x8, [sp, #-0x10]!` — the bug, verbatim.

**Rules that follow:**

- **A regression test is not done until the mutation run is recorded.** Break the
  fix, watch the test fail, restore. Reasoning about coverage is not a substitute
  — this caught the test's own author, in the same session that wrote the fix,
  with the mechanism fully understood.
- **Prefer encoding assertions over execution** for codegen. They cannot hang and
  run in microseconds.
- **A tripwire that costs minutes will not get run.** One draft spun forever: it
  installed a continuation into X30, destroying the test harness's own return
  address, so the continuation returned to itself. (The return-linkage test hung
  on a return-linkage bug — a fair summary of why this class is easy to write and
  hard to see.) Diagnose hangs with `sample`, not guesswork.
- Keep hardware-behaviour tests if they are free, but **label them** so nobody
  mistakes a green result for compiler coverage.

---

## 6. Emitter coverage still owed

`static_load` / `static_store` have **no differential coverage**. They were
suspected during one investigation and exonerated by measurement, but the gap is
real. `splat_vf` and the memory family are covered in
`test/goalc/test_arm64_differential_memory.cpp` (mutation-verified).

When translating SSE→NEON, double-check: min/max NaN propagation ordering,
denormal handling, integer↔float conversion rounding modes, and shuffle/permute
lane order. These produce plausible-looking output and only show up in play.

---

## 7. If you are adding Jak 3

The ARM64 backend is game-agnostic; nearly all work here was Jak-1-specific
*mirroring*. Expect:

1. **The seven bug-class instances above, again.** Jak 3's `goal_src` will have
   its own copies of the kernel asm-funcs. Run the §1 sweeps first — they are
   minutes of work and would have saved several sessions here.
2. **Heap and buffer constants** (§2). Jak 3's levels are larger than Jak 2's;
   audit anything sized against code output before debugging load failures.
3. **`jak3/` was deliberately not restored** in this fork — there are no Jak 3
   sources here. Start from upstream's Jak 3 tree, then apply the ARM64 deltas
   this repo makes to `jak1/`, using `git log --follow` on the kernel files as
   the checklist.
4. **Use both existing games as controls.** Nearly every fix here was found by
   asking "what does the *working* game do differently?" A three-way diff needs
   a third corner; with Jak 1 and Jak 2 both working, Jak 3 has two.

---

## Appendix: verified environment

- Apple M4 Pro, macOS. Preset `Release-macos-arm64-clang`.
- `goalc --target-arch arm64` writes to `out/<game>-arm64/`; the x86 suffix is
  empty. Never copy `out/<game>/iso` between architecture trees — it contains
  compiled code and will SIGILL.
- macOS `MAP_JIT` refuses write+execute; the W^X split is load-bearing. Read
  `esr.description` in a crash report before theorising: "(Instruction Abort)
  Permission fault" (page not executable) and "(Data Abort) ... write Permission
  fault" (page not writable) need opposite fixes.
- `timeout(1)` does not exist on macOS — background the run and `kill` it.
