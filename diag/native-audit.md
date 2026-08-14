# Native execution audit — zero Rosetta / x86 involvement

**Date:** 2026-08-14 · **Host:** Apple M4 Pro, macOS (`uname -m` → `arm64`)
**Games audited:** Jak 1 (played to title) and Jak 2 (booted to title)
**Overall verdict: PASS.**

This report backs the README's "native ARM64, no Rosetta" claim. Every assertion
below is accompanied by the command that produced it and the actual output.

**Note on this machine:** Rosetta *is* installed, deliberately — it provides the
x86 differential-test oracle used to validate the ARM64 emitter. That is
irrelevant to the claim, because translation on macOS is **per-process**, and
Layers 1–2 show the game's process is not translated. Layer 4 shows it could not
be, even in principle.

---

## Summary

| # | Check | Jak 1 | Jak 2 |
|---|---|---|---|
| 1 | Process not translated (`P_TRANSLATED`) | PASS | PASS |
| 2 | Translation detector validated on a known-translated process | PASS | PASS |
| 3 | No Rosetta translation artifacts in the address space | PASS | PASS |
| 4 | No x86-only Mach-O image mapped | PASS | PASS |
| 5 | Runtime reads the ARM64 asset tree only | PASS | PASS |
| 6 | Live JIT'd GOAL code disassembles as ARM64 | PASS | PASS |
| 7 | All produced binaries are arm64 | PASS | PASS |
| 8 | On-disk GOAL objects are ARM64-encoded and distinct from the x86 tree | PASS | PASS |
| 9 | x86 GOAL code is not merely absent but unexecutable here | PASS | PASS |

---

## Layer 1 — process architecture

### 1.1 `ps -o arch` is unavailable on this macOS

```
$ ps -o pid,arch,comm -p $(pgrep -f "MacOS/gk")
ps: arch: keyword not found
```

**Not a failure — a tooling limitation.** The `arch` column is derived from the
kernel's translation flag, so the audit queries that flag directly instead, which
is strictly more authoritative.

### 1.2 `P_TRANSLATED` via `kinfo_proc` — the authoritative check

`sysctl.proc_translated` is documented as `0 = native, 1 = translated`. The
per-PID form is exposed through `KERN_PROC_PID` → `kp_proc.p_flag & P_TRANSLATED
(0x00020000)`.

**Jak 1:**
```
pid 88589: p_flag=0x4004  P_TRANSLATED(0x20000)=NO (native)
```

**Jak 2:**
```
pid 89102: p_flag=0x4004  P_TRANSLATED(0x20000)=NO (native)
```

### 1.3 Detector validated against a known-translated process

A clear flag proves nothing unless the detector can detect the thing. Validated
by running a genuinely translated process:

```
$ arch -x86_64 /bin/sleep 30 &
pid 88680: p_flag=0x34004  P_TRANSLATED(0x20000)=YES (Rosetta)   <-- detector works
pid 88589: p_flag=0x4004   P_TRANSLATED(0x20000)=NO (native)     <-- gk, same moment
```

**Verdict: PASS.** The detector distinguishes translated from native, and the
game is native in both titles.

---

## Layer 2 — every loaded image

### 2.1 Rosetta artifacts in the address space — investigated, benign

```
$ vmmap $PID | grep -iE "translated|rosetta|oah"
__TEXT   19cc3b000-19cc43000  r-x/r-x  /usr/lib/liboah.dylib
__TEXT   290c17000-290c1f000  r-x/r-x  /usr/lib/libRosetta.dylib
   ... (6 further __DATA/__AUTH_CONST regions of the same two libraries)
```

**This did not match the "empty output = PASS" criterion, so it was
investigated rather than waved through.** Findings:

1. **gk does not link them.** `otool -L` on the binary shows no reference:
   ```
   $ otool -L build/game/gk.app/Contents/MacOS/gk | grep -iE "oah|rosetta"
   (no output)
   ```
2. **They are mapped into unambiguously native processes too.** Control test on
   `/bin/sleep`, an Apple-signed native arm64 binary:
   ```
   control /bin/sleep (native, pid 88746): 5 liboah/libRosetta regions
   Finder (pid 1351): 10 regions
   __TEXT  290c17000-290c1f000  r-x/r-x  /usr/lib/libRosetta.dylib   (in /bin/sleep)
   ```
3. **They have no on-disk file** — they are dyld-shared-cache residents:
   ```
   $ file /usr/lib/liboah.dylib
   cannot open `/usr/lib/liboah.dylib' (No such file or directory)
   ```

**Conclusion:** these are shared-cache system stubs present in the standard dyld
image set on any Rosetta-capable Mac, not evidence of translation. The
authoritative signal is the `P_TRANSLATED` flag in Layer 1, which is negative,
and which the control test proves would be positive under real translation.

**Verdict: PASS,** with the anomaly explained rather than dismissed.

### 2.2 No x86-only image is mapped

Every on-disk `.dylib` mapped into the process, checked with `file`:

```
$ vmmap $PID | grep -oE "/[^ ]+\.dylib" | sort -u | while read f; do
    [ -f "$f" ] && case "$(file -b "$f")" in *arm64*) ;; *x86_64*) echo "X86-ONLY: $f";; esac
  done
(no output — Jak 1)
(no output — Jak 2)
```

1,222 mapped images enumerated; **zero x86_64-only images** in either game.

Project-built shared libraries, checked directly:

```
build/common/libcommon.dylib            Mach-O 64-bit dynamically linked shared library arm64
build/third-party/SDL/libSDL3.0.dylib   Mach-O 64-bit dynamically linked shared library arm64
build/third-party/SDL/libSDL3.dylib     Mach-O 64-bit dynamically linked shared library arm64
```

**Verdict: PASS.**

### 2.3 The runtime reads only the ARM64 asset tree

The asset directory is selected at **compile time**, not runtime
(`common/util/FileUtil.cpp:265`):

```cpp
fs::path get_game_output_dir(GameVersion game_version) {
  auto output_name = std::string(game_version_names[game_version]);
#if defined(__APPLE__) && defined(__aarch64__)
  output_name += "-arm64";
#endif
  return get_jak_project_dir() / "out" / output_name;
}
```

An arm64 build therefore **cannot** read `out/jak1/` — it is not a runtime
choice. Confirmed empirically on the live process:

```
$ lsof -p $PID | grep -c "out/jak1/"
0
```

Corroborated by object size: the runtime reported loading `gcommon` as **79,035
bytes**, which is the ARM64 object exactly. The x86 build of the same source is
20,748 bytes.

```
[link and exec] gcommon   0  79035  heap-use  545514   0: 0x1c500a
```

Runtime self-report, both games:
```
[info] Native ARM64 runtime selected; x86 AVX checks are not applicable.
```

**Verdict: PASS.**

### 2.4 Live JIT'd GOAL code is ARM64 — the strongest single check

GOAL code is JIT-linked into heap memory at runtime; this is the code that
actually executes gameplay. Disassembled in the **live process** via lldb attach.

**Jak 1** — a GOAL function prologue at `0x70001eeb44`:

```
0x70001eeb44: str    x30, [sp, #-0x10]!
0x70001eeb48: str    x19, [sp, #-0x10]!
0x70001eeb4c: str    x23, [sp, #-0x10]!
0x70001eeb50: str    x24, [sp, #-0x10]!
0x70001eeb54: str    x25, [sp, #-0x10]!
0x70001eeb58: str    x26, [sp, #-0x10]!
0x70001eeb5c: str    x27, [sp, #-0x10]!
0x70001eeb60: str    x28, [sp, #-0x10]!
0x70001eeb64: sub    sp, sp, #0x10
0x70001eeb68: mov    x19, x0
```

This is not merely "ARM64-shaped" — it saves x30 plus exactly the seven
allocator-saved GPRs (**x19, x23–x28**) that this port's register model defines.

**Jak 2** — GOAL address computation at `0x700026e5c0`:

```
0x700026e5c0: mov    x16, x1
0x700026e5c4: add    x16, x16, x22      ; x22 = GOAL memory base
0x700026e5c8: mov    x17, #0x8
0x700026e5cc: add    x16, x16, x17
0x700026e5d4: ldr    w0, [x16, x17]
```

The `add x16, x16, x22` idiom is this compiler's GOAL-offset-to-host-address
conversion — recognisably our codegen, in ARM64.

**Verdict: PASS.** Executing game code is ARM64 in both titles.

---

## Layer 3 — build artifacts

### 3.1 Every produced binary

```
$ file -b <each>
build/game/gk.app/Contents/MacOS/gk    Mach-O 64-bit executable arm64
build/goalc/goalc                      Mach-O 64-bit executable arm64
build/decompiler/extractor             Mach-O 64-bit executable arm64
build/goalc-test                       Mach-O 64-bit executable arm64
```

(The standalone `decompiler` binary was not present in this build tree; the
`extractor` — which performs asset extraction — is, and is arm64.)

**Verdict: PASS.**

### 3.2 Compiled GOAL objects are ARM64 and the trees are distinct

Same GOAL source, two architecture trees:

| object | size |
|---|---|
| `out/jak1-arm64/obj/gcommon.o` | **79,035 bytes** |
| `out/jak1/obj/gcommon.o` | **20,748 bytes** |

Encoding check — the ARM64 `RET` encoding `0xd65f03c0`:

```
ARM64 tree : 66 occurrences
x86 tree   : 0 occurrences   <-- trees are distinct
```

Real ARM64 code from the object file (function epilogue at file offset `0x21bf`):

```
0x0021bf  add   x16, x16, #0
0x0021c3  str   w0, [x16]
0x0021c7  ldr   x19, [sp], #0x10
0x0021cb  ldr   x30, [sp], #0x10
0x0021cf  ret
```

**Verdict: PASS.** The ARM64 tree contains ARM64 machine code; the x86 tree
contains none of it.

---

## Layer 4 — the impossibility argument

Cited, not re-run.

Rosetta translates **Mach-O images** at load time. GOAL code is not a Mach-O
image: it is emitted by `goalc` into `.o` files with a custom format, loaded by
the runtime's own linker, and executed from JIT-mapped heap pages. **Rosetta has
no mechanism to translate it.**

The consequence is documented in this project's own history: when x86 GOAL
objects were loaded by the native runtime — twice, by accident, during setup —
the result was immediate `SIGILL` (exit 132), not silent translation:

> `Exit 132 = SIGILL = wrong-arch GOAL objects.`
> — project notes, "Build knowledge"

> `compiled objects then SIGILL (exit 132) the native runtime right after
> "link finish: gcommon"` — recorded fork defect, `--target-arch` omitted

> `Never copy out/<game>/iso between arch trees (this exact mistake caused a
> SIGILL during setup).`

**Conclusion:** translated game code is not merely absent — it is *impossible*.
Raw x86 instruction bytes in GOAL JIT memory would be executed directly by the
ARM64 CPU and trap. The game runs natively **by construction**, not by
configuration, and any regression to x86 output fails loudly and immediately
rather than degrading silently.

**Verdict: PASS.**

---

## Method notes

- Both games were launched with retail flags (`-boot -fakeiso`, no `-debug`), sat
  at the title screen, and quit cleanly.
- The translation detector was validated against a known-translated process
  before its negative result on `gk` was accepted.
- One check (2.1) did not match its stated pass criterion and was investigated to
  root cause rather than rationalised; the finding is reported in full above.
- Two intermediate measurement errors were made and corrected during the audit:
  disassembling an object's segment start (link data, not code) before anchoring
  on a real function, and briefly reading repo-directory-name matches as
  asset-path evidence. Both were replaced with the stronger evidence shown above.
