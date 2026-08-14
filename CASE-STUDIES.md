# Case studies: two long bug hunts

Companion to [PORTING-NOTES.md](PORTING-NOTES.md). Those are the conclusions;
these are the routes taken to reach them, including the wrong ones.

Published because the wrong turns are the useful part. Both of these bugs took
multiple sessions, and in both the *fastest* technique was tried late. If you are
debugging something similar, the value here is in the order things should have
been tried.

---

## Case study 1: the runaway stack descent

**Symptom.** After the intro cinematic, the game died with `SIGBUS`,
`EXC_BAD_ACCESS` at address `0x6ffffffffc` — one word below the GOAL memory base,
i.e. a deref of offset `-4`: the type word of a null pointer.

### The route taken

**Sessions 1–3 — the fault site.** Mapped the faulting PC to a function by
matching emitted segment order against source lambda order. Landed on a `:trans`
behavior making a method call on `*default-dead-pool*`, which read as 0.

**Session 4 — the symbol is not unbound; it is a duplicate.** The symbol's *name
string* had been overwritten in place, hash field intact. `find_symbol_from_c`
probes by hash then confirms with `strcmp` — so the probe hash-matched, `strcmp`
failed, the probe walked on, and `intern_from_c` **minted a duplicate slot** with
value 0. Every object linked afterwards bound the empty duplicate.

Bracketed exactly: every object through `title-obs` resolved the correct slot;
`villagep-obs` and everything later got the duplicate.

**Session 5 — what wrote it.** A hardware watchpoint caught the instruction on
the first attempt:

```
str x0, [sp, #-0x10]!      sp = 0x70001f4700, x0 = return-from-thread
```

Ordinary stack traffic — a descending stack that had run off the bottom of the
8 MB dram stack onto the symbol-name heap directly beneath it. Memory at SP
showed one 8-byte push per 16 bytes with original strings surviving in between.

Ruled out along the way, each with a measurement: heap reuse (one allocation
covers the address, and it is the one that *creates* the string), bump-pointer
rewind (zero rewinds in a full boot), a mis-sized dram stack (allocation and
constant agree exactly at `0x800000`), and backup-stack overrun (the debug check
is live and printed zero times).

A 64 kB guard page below the stack did not stop it — the guard filled with the
same frame record throughout. **That was the clue that mattered**: no fixed slack
contains an unbounded descent.

**Session 6 — the actual cause, found by reading source.** The self-kill branch of
`(method deactivate process)`:

```lisp
;; TODO: replace with abandon.
(let ((temp (the uint return-from-thread)))
  (rlet ((off :reg r15 :type uint)) (+! temp off) (.push temp) (.ret)))
```

On x86, `ret` pops the pushed address and control transfers to
`return-from-thread`. **On ARM64, `ret` branches to X30 and ignores the stack** —
so the method returned to *its own caller*, which is the child/brother walk twelve
lines above, which recursed forever, leaking one 16-byte push per trip.

The correct ARM64-guarded form, `abandon-thread`, **was already in the file with
zero call sites** — dead code, with a stale `;; TODO` marking the exact line that
needed it. Jak 2 called it. A second identical site existed in `gstate.gc`.

### What this should have cost

The watchpoint capture was correct in every particular and still did not yield
the cause, because it identified the **writer** (the push) rather than the
**control-flow error** that made the writer run forever.

A three-way static diff of `deactivate` against Jak 2 and upstream would have
found it in minutes. **When a capture explains the mechanism but not the reason,
switch to static diff rather than tightening the instrument.**

---

## Case study 2: the null jump, and five wrong hypotheses

**Symptom.** `EXC_BAD_ACCESS (code=1, address=0x0)`, `pc = 0`, `lr = 0`, zero
recoverable frames. Reproducible, with a near-identical register file every run.

This one is worth reading mostly as a sequence of falsifications.

### Hypothesis 1 — a null particle group (REFUTED by measurement)

Static reading found an unguarded virtual method call:
`(create-launch-control group-part-save-icon self)` where the group is a plain
table read. If entry 656 were `#f`, the dispatch would read a method from `s7`'s
type word, get 0, and jump — matching `pc=0` exactly.

A census probe over the whole 1024-entry table showed slot 656 **valid and stable
at `0x2ae8864` on 20 of 21 samples**, including right up to the crash window. The
single empty reading was from before the defining object had linked, which is
correct.

Refuted. This also exonerated `static_store`, which had been the leading
architecture-specific explanation.

### Hypothesis 2 — a test/jump race on the hook slot (REFUTED by a guard)

The trans/post hook slot is read twice — once by the `cond` test, once as the
argument to `reset-and-call`. With no CSE pass in the compiler, that is two real
loads on *every* target, so the window is real.

A guard captured both values at every dispatch and reported when they differed.
It was validated by a one-shot "ALIVE" line on the first healthy dispatch, then
ran through a full crash: **zero hits**. Tested value always equalled jumped
value, and the hook was never `#f`.

Refuted — and because the guard covered the *window* rather than any particular
writer, it eliminated three candidate mechanisms at once.

### Hypothesis 3 — a virgin thread resumed without initialisation (REFUTED)

Attractive because it explained `pc=0` *and* `lr=0` from one cause: a
zero-initialised save area faithfully restored.

No tag field was needed to test it — a virgin thread is self-identifying, since
`set-to-run` always writes both `pc` and `rreg 0`. A guard at the single resume
choke point fired its ALIVE line and then stayed silent through a crash.

Refuted.

### Hypothesis 4 — a save-area layout asymmetry (REFUTED by audit)

The 5-vs-7 saved-GPR mismatch had already caused three bugs, so a writer/reader
offset mismatch in the `cpu-thread` save area was a natural suspect.

Audit: `rreg` is **already 7 entries upstream**, so the ARM64 expansion shifted
nothing; offsets are compiler-enforced by `:offset-assert`; every writer/reader
pair covers `rreg 0..6` with matching roles; and `grep` showed the writer set is
exactly two GOAL functions, with the C side never touching the save area.

Refuted, symmetric everywhere.

### Hypothesis 5 — missing x30 linkage at thread launch (REFUTED at machine level)

The launch-side twin of case study 1. Checked in the emitted binary rather than
the source: `gkernel` seg0 contains **7 `br` sites and 7 `mov x30, x8`**,
correctly paired. `set-to-run-bootstrap` loads X30 immediately before its jump.
The two unpaired `br`s are a resume-to-saved-pc and a tail call — correct as-is.

Refuted.

### The actual cause

`enter-state`'s trampoline:

```lisp
(rlet ((temp)          ; <-- NO :reg CONSTRAINT
       (func) ...)
  (.mov func (-> new-state code))
  (.mov temp return-from-thread-dead)
  (.push temp)         ; intended to install the return trampoline
  (.jr func))
```

`IR_AsmPush::do_codegen_arm64` lowers `.push` to `mov x30, x8` **only when the
source register is X8**. An unconstrained `temp` got x4, so the push emitted a
real `str x4, [sp, #-0x10]!` and **X30 was never loaded**.

Jak 2 had `(arm-temp :reg rax)` and an `#if ARM64_TARGET` routing the push through
it. Jak 1 had neither — and this exact delta had been *noticed and noted* several
sessions earlier as "an unexplained jak1-vs-jak2 difference, should be checked".
It was the bug.

Verified in machine code: before the fix, zero `mov x30,x8` in the object; after,
one correctly paired with the `br`.

### Corrections made along the way

Recorded because the mistakes were instructive:

- **`x16 = 0xeb70` read as a method slot.** It is not — `0xeb70/8 = 7534`, no such
  method index. X16 is ARM64's address-computation scratch register. Every
  conclusion resting on that reading was void.
- **A load-map lookup ran off the end**, reporting a heap address as
  `gkernel +0x881ce4` in an object ~0x30000 long. Always check
  `offset < object length`.
- **"lr = 0 implies x8 = 0"** — disproven by a later capture measuring `x8`
  non-zero *with* `lr = 0` in the same stop.
- **A watchpoint armed at a wrong address** (no GOAL base added, and 0x400 high),
  making its silence meaningless. lldb reported the arm as successful.
- **`FUNCOFF` offsets do not match the emitted `.o`** and are inconsistently
  shifted — deltas differing in both magnitude and sign against seven independent
  anchors. Never accept a `FUNCOFF` identification without an independent anchor.

---

## Case study 3: the crash that never happened

Brief, but the most embarrassing and probably the most useful.

A play-test was reported as failing at a specific missing-asset stall. Two
sessions of instrumentation were planned around that trigger — including a guard
built specifically for it.

Reading the log line-by-line showed the game had **survived** the stall: the
warnings appeared at line 1187, and the log continued to line 1268, including the
successful link of the very asset in question and an entire subsequent level load.
The real failure was a heap overflow **76 log lines and about three minutes
later**, in a different subsystem.

Two rules came out of it:

- **"It crashed after doing X" is not "it crashed at X".** Locate the death point
  by line number relative to the suspect event before accepting a trigger
  attribution.
- **Check whether a "missing file" is even a defect.** The requested asset shipped
  on no US disc, appears in no object table, and the loader's handling of it is a
  designed non-fatal path that every player traverses. Two independently-mastered
  discs confirmed the gap.

---

## What the whole exercise suggests about method

Ranked by what actually produced results:

1. **Three-way static diff** (working game vs broken game vs upstream). Found
   five of the seven bug-class instances, in minutes each.
2. **Measurement over inference.** Every hypothesis that survived contact was one
   that got measured; several plausible ones died on first contact with a number.
3. **Guards that cover a *window* rather than a suspect.** One validated guard
   eliminated three candidate mechanisms simultaneously.
4. **Hardware watchpoints** — excellent at finding *writers*, which is not always
   the same as finding *causes*.
5. **Reading the emitted machine code.** Settled two questions that source
   reading could not, because the bug was in a codegen *decision*.

And the discipline that underwrites all of it: **validate the instrument before
trusting its silence, and break the fix to confirm the test catches it.**
