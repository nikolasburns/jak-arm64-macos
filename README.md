# OpenGOAL on Apple Silicon — native ARM64 macOS, Jak 1 **and** Jak 2

Run **Jak and Daxter: The Precursor Legacy** and **Jak II** natively on Apple
Silicon. No Rosetta, no translation layer — the GOAL compiler emits ARM64
directly and the runtime executes it.

That claim is audited, not asserted: see
**[diag/native-audit.md](diag/native-audit.md)** for a nine-check verification
covering process translation flags, every mapped Mach-O image, live
disassembly of JIT'd GOAL code in both running games, and the build artifacts —
each with the command and its actual output.

**Status**

| Game | State |
|---|---|
| **Jak 1** | **Playable.** Boots, intro cutscene, Geyser Rock and Village 1 load and play. Gameplay validation in progress. |
| **Jak 2** | Boots to title / attract. Full playthrough not yet validated. |

---

## Lineage

This project stands on two others, and the history here preserves both:

- **[open-goal/jak-project](https://github.com/open-goal/jak-project)** — the
  decompilation, the GOAL compiler, and the runtime. Everything here is
  downstream of that work.
- **[DiMiTriFrog/jak2-macos-arm64](https://github.com/DiMiTriFrog/jak2-macos-arm64)**
  — the original ARM64 backend: `IGenARM64`, the ARM64 register model, the
  C↔GOAL bridges, and the Jak 2 port. That fork made native Apple Silicon
  possible.

**What this repository adds:**

1. **Jak 1 support on ARM64** — the upstream Jak 1 game restored into the ARM64
   fork (which had removed it), and the compiler/runtime gaps that surfaced only
   once Jak 1 exercised them.
2. **Seven bug fixes** in the shared ARM64 kernel path, each documented with the
   evidence that found it (see below).
3. **Regression coverage** for the bug class that produced most of them.

The full commit history from upstream through the ARM64 fork to this work is
intact — `git log --follow` on any kernel file shows the whole chain.

---

## What was actually wrong (and why it may help your port)

Nearly every bug found here belonged to one class, which we came to call
**"x86 semantics in ARM64 clothing"**: GOAL assembly-form code that is correct
only because of an *x86 instruction's side effects*, compiled unchanged for
ARM64.

Seven instances, all in code that Jak 2 had already ported correctly and Jak 1's
mirror had not:

| # | Site | The x86 assumption |
|---|---|---|
| 1 | C→GOAL bridges | saved-register *role names* |
| 2 | `gkernel` context switches | same role names |
| 3 | catch/throw saved set | same role names |
| 4 | `cpu-thread` backup stacks | x86-sized frames |
| 5 | `deactivate` + `enter-state` | `ret` consumes a pushed address |
| 6 | `enter-state` trampoline | `.push` reaches x30 only from the `rax` role |
| 7 | level-heap sizing | heap sized to x86 code density |

The two that cost the most: **ARM64 has seven allocator-saved GPRs where GOAL's
x86 vocabulary names only five**, and **`ret`/`br` branch to X30 and ignore the
stack**, so the x86 "push the return address, then return" idiom silently does
nothing.

There is now an encoding-level tripwire for the whole family in
[`test/goalc/test_arm64_continuation_handoff.cpp`](test/goalc/test_arm64_continuation_handoff.cpp).
It drives the compiler's IR directly, asserts on emitted bytes, has a table row
per real call site, and is mutation-verified — reverting the fix makes it fail
with the offending instruction word in the message.

**Two documents carry the detail:**

- **[PORTING-NOTES.md](PORTING-NOTES.md)** — the engineering reference. The bug
  class, ARM64 traps, code-density findings, techniques for debugging a JIT'd
  Lisp runtime, and a section on **adding Jak 3**.
- **[CASE-STUDIES.md](CASE-STUDIES.md)** — two multi-session bug hunts written up
  in full, *including the five falsified hypotheses and the misreadings*. The
  wrong turns are the point: they show which techniques pay off and in what
  order.

---

## Requirements

- Apple Silicon Mac (developed on M4 Pro), macOS.
- `brew install cmake ninja go-task`
- **Your own legally-purchased game discs.** No assets are included and none can
  be. See [Legal](#legal).

## Build

```sh
cmake -B build --preset=Release-macos-arm64-clang
cmake --build build --parallel "$(sysctl -n hw.logicalcpu)"
```

## Extract assets from your disc

Copy your disc image contents into `iso_data/jak1/` (or `iso_data/jak2/`), then:

```sh
./build/decompiler/extractor --folder --decompile --game jak1 iso_data/jak1
```

Extraction runs levels **in parallel and out of order** — an interrupted run
leaves an arbitrary subset with no error. If assets later appear missing, check
that all level `.fr3` files are present before assuming a code problem.

## Compile the game

**The `--target-arch arm64` flag is required.** Without it the compiler silently
emits x86-64, and the native runtime dies with `SIGILL` (exit 132) shortly after
boot. (Note: the bundled `task repl` shortcut omits this flag — use the direct
command.)

```sh
./build/goalc/goalc --user-auto --game jak1 --target-arch arm64 --cmd "(mi)"
```

Use `--cmd` rather than piping — `echo "(mi)" | goalc` wedges the REPL at 99% CPU
*after* a successful build, which looks exactly like a hung compile.

ARM64 output goes to `out/jak1-arm64/`; x86 output goes to `out/jak1/`. **Never
copy `out/<game>/iso` between architecture trees** — it contains compiled code.

## Run

```sh
./build/game/gk.app/Contents/MacOS/gk -v --game jak1 -- -boot -fakeiso
```

Exit codes worth knowing: **132** = SIGILL (wrong-arch objects — you missed
`--target-arch arm64`), **134** = missing assets, **138** = SIGBUS,
**139** = SIGSEGV. A clean quit exits 0 and logs `GOAL Runtime Shutdown`.

For Jak 2, substitute `jak2` throughout.

---

## Known issues

- **Jak 2** boots to title and attract; a full playthrough has not been
  validated. Expect the same class of bugs Jak 1 hit, in Jak 2-specific code.
- **Jak 1 gameplay validation is in progress.** Later levels are expected to work
  but have not all been reached.
- One benign upstream oddity, so it is not mistaken for a port defect: Jak 1
  requests `sage-intro-sequence-b`, which shipped on **no US disc** and appears in
  no object table. The loader's handling is a designed non-fatal path — the
  warnings during the sage's intro are normal and every x86 player sees them too.

## Contributing

Bug reports from Apple Silicon play-testing are the most useful thing right now,
especially for Jak 2 and for Jak 1 past Village 1. Include the exit code and the
tail of the log.

If you are extending this to Jak 3 or another architecture, read
[PORTING-NOTES.md](PORTING-NOTES.md) §7 first — it lists what to sweep for
before you start debugging.

## Legal

OpenGOAL is a **decompilation project**. This repository contains **no game
assets** — no ISOs, no textures, no audio, no level data — and none will be
accepted. You must own and extract your own copy of each game.

Jak and Daxter is a trademark of Sony Interactive Entertainment. This project is
not affiliated with or endorsed by Sony or Naughty Dog.

## License

Inherits upstream OpenGOAL licensing. See [LICENSE](LICENSE).
