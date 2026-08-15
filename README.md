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

## Engineering notes

Two documents carry the detail of what the port actually involved:

- **[PORTING-NOTES.md](PORTING-NOTES.md)** — the engineering reference. The
  recurring bug class, ARM64 traps, code-density findings, techniques for
  debugging a JIT'd Lisp runtime, and a section on **adding Jak 3**.
- **[CASE-STUDIES.md](CASE-STUDIES.md)** — two multi-session bug hunts written up
  in full, *including the falsified hypotheses and the misreadings*. The wrong
  turns are the point: they show which techniques pay off and in what order.

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

### Optional: HD texture packs

Upstream OpenGOAL supports replacing textures at extraction time, and that path
works unchanged on ARM64. Two community ESRGAN upscale packs cover the games in
full:

| Game | Pack | Source |
|---|---|---|
| Jak 1 | *Jak and Daxter Reloaded in HD* | [NexusMods (OpenGOAL) mod 10](https://www.nexusmods.com/opengoal/mods/10) |
| Jak 2 | *OpenGOAL Jak 2 HD Texture Pack* (MIT) | [github.com/Aloqas/OpenGOAL-Jak2-HD-Texture-Pack](https://github.com/Aloqas/OpenGOAL-Jak2-HD-Texture-Pack) |

**This repository ships no textures** — install a pack yourself if you want one.
Drop its contents into `custom_assets/<game>/texture_replacements/<tpage>/…`
(both packs already use that layout) and re-run the extractor. Replacement is
keyed on `<tpage_name>/<texture_name>.png`, with an `_all/` directory as a
fallback for flat packs. `custom_assets/*/texture_replacements/` is gitignored.

Notes from installing both:

- **Expect a much bigger `fr3/` tree.** Jak 1 grew 201 MB → 2.1 GB (~10×);
  levels individually range from 4× to 20×. Budget disk accordingly.
- **The texture pass is single-threaded.** `extract_level.cpp` forces
  `num_workers = 1` whenever replacements are active, so this run cannot be
  parallelised — but it is still fast (~30 s for Jak 1 on an M4 Pro).
- **Skip animated textures, sky domes and eye textures** if a pack includes
  them: those are composited or cycled at runtime and break when upscaled. Both
  packs above already exclude the animated set, so no manual pruning is needed.
- **Strip editor residue** (`.png~`, `.svg`, `desktop.ini`) before extracting.
- Remember the copy step below — the decompiler writes to `out/<game>/fr3`
  while the runtime reads `out/<game>-arm64/fr3`.

```sh
cp out/jak1/fr3/*.fr3 out/jak1-arm64/fr3/
```

`fr3` files are architecture-neutral, so copying them between trees is safe.
**`iso/` is not** — it contains compiled code and must never be copied across
architectures.

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

## Package as a standalone `.app`

Wrap a compiled game into a double-clickable app that no longer needs this
checkout:

```sh
./scripts/package-app.sh jak1        # -> /Applications/Jak 1.app
./scripts/package-app.sh jak2        # -> /Applications/Jak 2.app
```

Add a second argument to install elsewhere, or `--no-bundle-deps` to skip dylib
packaging — faster, but the result runs only on this machine:

```sh
./scripts/package-app.sh jak1 ~/Applications --no-bundle-deps
```

The bundle embeds the game data, shaders, assets and every non-system dylib, and
is ad-hoc signed with the JIT entitlement `gk` needs. Copy it to any Apple
Silicon Mac and it runs; no build tree or Homebrew required. If Gatekeeper blocks
the first launch, approve it in **System Settings → Privacy & Security** or run
`xattr -dr com.apple.quarantine "/Applications/Jak 1.app"`. Don't launch it with
`sudo`.

Saves and settings live in `~/Library/Application Support/OpenGOAL/<game>/`,
shared with the plain `gk` binary.

Expect several gigabytes — roughly 3.7 GB for Jak 1 and 9.6 GB for Jak 2 with
the HD packs installed. Bundles are snapshots: re-run the script after any
`(mi)` or asset change. They are **not** notarized and are not for distribution,
since the embedded assets come from your own disc.

---

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
