# ANGLE binaries (libEGL / libGLESv2)

Prebuilt ANGLE dylibs for the `angle-backend` GLES 3.0 path. **The game does not
boot on `GK_GFX_BACKEND=angle` without these**, and they are now the only
surviving artifact of the ANGLE build — the 12 GB source checkout they came from
lived in `/private/tmp` and has been purged.

| | |
|---|---|
| ANGLE revision | `aa192212af54` (2026-08-14) |
| Built for | macOS arm64 (Apple Silicon) |
| Backend | Metal (`ANGLE_DEFAULT_PLATFORM=metal`, set in code) |
| Size | ~16 MB total |

## Self-authentication

These binaries state their own provenance at runtime — no checksum file to drift
out of date. Every context creation logs the revision inside `GL_VERSION`:

```
OpenGL context version: OpenGL ES 3.0 (ANGLE 2.1.1 git hash: aa192212af54)
OpenGL context renderer: ANGLE (Apple, ANGLE Metal Renderer: Apple M4 Pro, ...)
```

If the hash in a boot log does not match the table above, the process loaded a
different ANGLE than the one committed here. Check that line before diagnosing
anything backend-shaped.

Two things that line also settles, both of which have bitten before:

- **`ANGLE Metal Renderer` must appear.** ANGLE self-selects its backend, and
  its default here is the *GL* backend, which produces a perfectly working GLES
  3.0 context that reads `OpenGL 4.1 Metal - 90.5` — i.e. ANGLE emulating GLES
  on top of the very Apple GL driver ANGLE exists to replace. "The shim loaded"
  is not the result; the renderer string is.
- **A symbol exported by these dylibs is not a function this context supports.**
  `libGLESv2` exports `glMultiDrawElementsANGLE` and the string
  `GL_ANGLE_multi_draw`, yet the live Metal context advertises no `multi_draw`
  extension at all and the call returns `GL_INVALID_OPERATION`. Ask the running
  context (`glGetString(GL_EXTENSIONS)`), never `nm` or `strings`.

## How they are loaded

Nothing links against them. `gk`'s `otool -L` has zero EGL/GLES/ANGLE entries.
SDL `dlopen`s them by name, and `set_angle_library_hints()`
(`game/graphics/pipelines/opengl.cpp`) points SDL's `SDL_HINT_EGL_LIBRARY` /
`SDL_HINT_OPENGL_LIBRARY` at this directory via `GK_ANGLE_DIR`:

```sh
GK_GFX_BACKEND=angle GK_ANGLE_DIR="$PWD/third_party/angle-bin" \
  ./build/game/gk.app/Contents/MacOS/gk -v --game jak1 -- -boot -fakeiso
```

`libEGL.dylib`'s install name is the relative `./libEGL.dylib`, so
**`GK_ANGLE_DIR` must be absolute.** Without `GK_GFX_BACKEND=angle` the runtime
uses AppleGL and never touches this directory.

## Patches

`patches/` holds one **falsified** candidate fix, kept only as a record.

| patch | targets | status |
|---|---|---|
| `0001-metal-invalidate-state-when-flush-ends-render-encoder.patch` | `aa192212af54` | **built, tested, DOES NOT FIX ANYTHING — do not apply** |

**These dylibs are stock and should stay stock.** The crash `0001` was written to
fix turned out to be ours, not ANGLE's: six background shaders declared a vertex
attribute `int` while feeding it `GL_UNSIGNED_SHORT` data, which Metal rejects at
pipeline creation. Fixed in `66d3b8b6ce`. See `diag/session312/FINDING.md`.

## Rebuilding

Only needed to move to a newer ANGLE revision. ~22 min, ~12 GB checkout,
following ANGLE's `doc/DevSetup.md` — plus two macOS prerequisites that its
setup doc does not mention, both fixable without `sudo`:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer  # not CommandLineTools
xcodebuild -downloadComponent MetalToolchain                     # Xcode 26 omits it (688 MB)
```

Beware `angle_enable_gl=false` in `args.gn`: `gni/angle.gni` derives
`angle_enable_essl` from it, so disabling the GL backend silently disables the
**output translator**, which then exits rc=1 on every input and prints usage.
