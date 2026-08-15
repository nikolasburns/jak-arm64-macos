#!/usr/bin/env bash
set -euo pipefail

# Build a standalone, double-clickable macOS .app for one game.
#
#   scripts/package-app.sh jak1 [output_dir]
#   scripts/package-app.sh jak2 [output_dir]
#   scripts/package-app.sh jak3 [output_dir]
#
# The bundle embeds the compiled game data from out/<game>-arm64, so it runs
# with no dependency on this checkout. That data is derived from the user's own
# discs: the resulting .app is FOR PERSONAL USE and must not be distributed.
# This script itself contains no game data and is safe to commit.
#
# Signing is ad-hoc (-s -). No notarization: that would require distribution.

usage() {
  cat >&2 <<USAGE
usage: $0 {jak1|jak2|jak3} [output_dir] [--no-bundle-deps]

  output_dir          where to write the .app (default: /Applications)
  --no-bundle-deps    skip dylib packaging. Faster, but the bundle then runs
                      ONLY on this machine and only while the build tree
                      exists. For dev-machine repackaging only.

By default every non-system dylib gk needs — including transitive and
Homebrew-absolute ones such as OpenSSL behind libcurl — is copied into
Contents/Frameworks and rewritten to @rpath, so the .app runs on any
Apple Silicon Mac.
USAGE
  exit 2
}

game=""
output_dir=""
bundle_deps=1
for arg in "$@"; do
  case "${arg}" in
    --no-bundle-deps) bundle_deps=0 ;;
    -h|--help)        usage ;;
    -*)               echo "error: unknown option ${arg}" >&2; usage ;;
    jak1|jak2|jak3)   game="${arg}" ;;
    *)                output_dir="${arg}" ;;
  esac
done
[[ -z "${game}" ]] && usage

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${output_dir:=/Applications}"

case "${game}" in
  jak1) app_name="Jak 1";     bundle_id="dev.nboily.jak1" ;;
  jak2) app_name="Jak 2";     bundle_id="dev.nboily.jak2" ;;
  jak3) app_name="Jak 3";     bundle_id="dev.nboily.jak3" ;;
esac

app="${output_dir}/${app_name}.app"
contents="${app}/Contents"
macos_dir="${contents}/MacOS"
data_dir="${contents}/Resources/data"
src_app="${project_root}/build/game/gk.app"
src_gk="${src_app}/Contents/MacOS/gk"
out_tree="${project_root}/out/${game}-arm64"

if [[ "$(uname -m)" != "arm64" ]]; then
  echo "error: must run on an arm64 host" >&2
  exit 2
fi
if [[ ! -x "${src_gk}" ]]; then
  echo "error: no gk binary at ${src_gk} (build first)" >&2
  exit 2
fi
if [[ "$(lipo -archs "${src_gk}")" != "arm64" ]]; then
  echo "error: gk is not pure arm64" >&2
  exit 4
fi
if [[ ! -d "${out_tree}/iso" ]]; then
  echo "error: missing compiled data ${out_tree}/iso — run (mi) first" >&2
  exit 2
fi
# /Applications is the default but is not always writable without elevation;
# say so plainly instead of failing partway through a multi-GB copy.
if ! mkdir -p "${output_dir}" 2>/dev/null || [[ ! -w "${output_dir}" ]]; then
  echo "error: ${output_dir} is not writable." >&2
  echo "       Re-run with sudo, or pass another directory:" >&2
  echo "         $0 ${game} ~/Applications" >&2
  exit 2
fi

echo "==> packaging ${app_name}"
rm -rf "${app}"
mkdir -p "${macos_dir}" "${contents}/Resources"

# --- executable + its dylibs -------------------------------------------------
# CMake does NOT create Contents/Frameworks: the freshly built gk resolves its
# ~16 dylibs through absolute LC_RPATH entries pointing into build/. Copying the
# binary alone therefore produces a bundle that only works while this checkout
# exists — it looks self-contained and is not. package_macos_arm64.sh already
# implements recursive dependency copying + @rpath rewriting, so delegate to it
# and then verify no absolute repo path survives.
cp "${src_gk}" "${macos_dir}/gk"
mkdir -p "${contents}/Frameworks"

frameworks_dir="${contents}/Frameworks"
copied=""

read_rpaths() {
  otool -l "$1" | awk '/LC_RPATH/{c=1;next} c&&/path /{sub(/^.*path /,"");sub(/ \(offset.*$/,"");print;c=0}'
}

# Every real LC_RPATH seen on an ORIGINAL binary, in first-seen order. This is
# the dyld search path for the whole dependency graph, and it must be collected
# BEFORE normalise_rpaths() strips the build-tree entries off the copy --
# otherwise a nested dependency has nothing left to resolve against.
rpath_search_list=""

record_rpaths() {
  local bin="$1" rp
  while IFS= read -r rp; do
    [[ -z "${rp}" ]] && continue
    [[ "${rp}" == @loader_path/* || "${rp}" == @executable_path/* ]] && continue
    [[ ";${rpath_search_list};" == *";${rp};"* ]] && continue
    rpath_search_list="${rpath_search_list}${rp};"
  done < <(read_rpaths "${bin}")
}

normalise_rpaths() {
  local bin="$1" rp
  while IFS= read -r rp; do
    [[ -z "${rp}" || "${rp}" == "@loader_path/../Frameworks" ]] && continue
    install_name_tool -delete_rpath "${rp}" "${bin}" 2>/dev/null || true
  done < <(read_rpaths "${bin}")
  if ! read_rpaths "${bin}" | grep -Fxq '@loader_path/../Frameworks'; then
    install_name_tool -add_rpath '@loader_path/../Frameworks' "${bin}" 2>/dev/null || true
  fi
}

# install_name_tool edits must be checked. A silently failed rewrite leaves an
# absolute path in place and produces exactly the half-portable bundle this
# whole step exists to prevent.
retarget() {
  local from="$1" to="$2" bin="$3" out
  out="$(install_name_tool -change "${from}" "${to}" "${bin}" 2>&1)" || {
    echo "error: install_name_tool -change ${from} -> ${to} failed on ${bin}" >&2
    [[ -n "${out}" ]] && echo "       ${out}" >&2
    exit 3
  }
}

# Resolve an @rpath dependency the way dyld does: search the binary's OWN
# LC_RPATH entries, in order, and take the first hit.
#
# This used to be `find "${project_root}/build" -name "${name}" -print -quit`,
# which returns whatever copy the filesystem walk reaches first and ignores
# rpath order entirely. A leftover nested build tree (build/Release/bin, from a
# different CMake preset) held a MONTHS-OLD libcommon.dylib, and the bundles
# shipped it instead of build/common/libcommon.dylib. The stale copy still had
# the pre-Jak1 font bank assert, so opening the in-game Display menu -- which
# calls get_font_bank_from_game_version() to encode each monitor's name --
# aborted the game. gk itself was current, so every check against gk looked
# clean and the bug read as a packaging mystery.
#
# Two properties matter and neither is optional:
#   1. rpath ORDER decides, so the bundle gets what dyld would have loaded.
#   2. Duplicates outside the winning rpath are reported, not silently ignored.
resolve_rpath_dep() {
  local bin="$1" name="$2" rp resolved="" first=""
  # Search the accumulated rpath list, in order. Using the recorded list rather
  # than the binary's current LC_RPATHs matters because a dylib already copied
  # into Frameworks has had its build-tree rpaths stripped by normalise_rpaths.
  local rest="${rpath_search_list}"
  while [[ -n "${rest}" ]]; do
    rp="${rest%%;*}"
    rest="${rest#*;}"
    [[ -z "${rp}" ]] && continue
    if [[ -f "${rp}/${name}" ]]; then
      resolved="${rp}/${name}"
      break
    fi
  done

  if [[ -z "${resolved}" ]]; then
    # Fall back to a build-tree search, but make ambiguity fatal rather than
    # arbitrary: if several copies exist we cannot know which dyld meant.
    local -a hits=()
    while IFS= read -r first; do
      [[ -n "${first}" ]] && hits+=("${first}")
    done < <(find "${project_root}/build" -name "${name}" -type f 2>/dev/null | sort)
    if [[ ${#hits[@]} -gt 1 ]]; then
      echo "error: ${name} is ambiguous -- ${#hits[@]} copies under build/ and none on" >&2
      echo "       an LC_RPATH of ${bin}:" >&2
      printf '         %s\n' "${hits[@]}" >&2
      echo "       Remove the stale build tree(s) or rebuild, then re-run." >&2
      exit 3
    fi
    resolved="${hits[0]:-}"
  fi
  printf '%s' "${resolved}"
}

bundle_binary() {
  local bin="$1" dep name src
  codesign --remove-signature "${bin}" 2>/dev/null || true
  # Record before normalising: normalise_rpaths deletes the build-tree entries,
  # and nested dependencies still need them to resolve.
  record_rpaths "${bin}"
  normalise_rpaths "${bin}"
  while IFS= read -r dep; do
    [[ -z "${dep}" ]] && continue
    # System libraries are present on every macOS install and are never copied.
    case "${dep}" in /System/*|/usr/lib/*|/usr/lib) continue ;; esac
    name="${dep##*/}"

    # A library already copied still needs THIS binary's reference rewritten.
    # Skipping the rewrite here was the original defect: libcurl kept an
    # absolute /opt/homebrew/... path to libcrypto, so the bundle ran only on a
    # machine that happened to have that exact Homebrew install.
    if [[ ";${copied};" == *";${name};"* ]]; then
      [[ "${dep}" != "@rpath/${name}" ]] && retarget "${dep}" "@rpath/${name}" "${bin}"
      continue
    fi

    # Resolve the real file. @rpath deps come from the build tree; absolute
    # deps (Homebrew: openssl, zstd, nghttp2) are taken from where they point.
    if [[ "${dep}" == @rpath/* ]]; then
      src="$(resolve_rpath_dep "${bin}" "${name}")"
    elif [[ -f "${dep}" ]]; then
      src="${dep}"
    else
      src="$(resolve_rpath_dep "${bin}" "${name}")"
    fi
    if [[ -z "${src}" || ! -f "${src}" ]]; then
      echo "error: cannot resolve dependency ${dep} of ${bin}" >&2
      exit 3
    fi

    # The ORIGINAL carries the build-tree rpaths its own dependencies need;
    # the copy is about to lose them to normalise_rpaths.
    record_rpaths "${src}"

    cp -L -f "${src}" "${frameworks_dir}/${name}"
    chmod u+w "${frameworks_dir}/${name}"
    codesign --remove-signature "${frameworks_dir}/${name}" 2>/dev/null || true
    if ! install_name_tool -id "@rpath/${name}" "${frameworks_dir}/${name}" 2>/dev/null; then
      echo "error: install_name_tool -id failed on ${name}" >&2
      exit 3
    fi
    retarget "${dep}" "@rpath/${name}" "${bin}"
    copied="${copied}${name};"
    bundle_binary "${frameworks_dir}/${name}"
  done < <(otool -L "${bin}" | tail -n +2 | sed -E 's/^[[:space:]]+([^[:space:]]+).*/\1/')
}

if [[ ${bundle_deps} -eq 1 ]]; then
  echo "    bundling dylibs"
  bundle_binary "${macos_dir}/gk"
else
  echo "    WARNING: --no-bundle-deps given; dylibs are NOT packaged." >&2
  echo "             This bundle will run ONLY on this machine, and only" >&2
  echo "             while ${project_root}/build exists." >&2
fi

# --- data/ -------------------------------------------------------------------
# try_get_data_dir() (common/util/FileUtil.cpp) looks for a "data" directory
# BESIDE the executable and, when found, uses it as get_jak_project_dir().
# Everything below was verified empirically by booting from outside the repo;
# a purely static reading of the source misses the shaders directory.
echo "    copying data (this is the slow part)"
mkdir -p "${data_dir}/game/assets" \
         "${data_dir}/game/graphics/opengl_renderer" \
         "${data_dir}/goal_src" \
         "${data_dir}/out"

cp -R "${project_root}/game/graphics/opengl_renderer/shaders" \
      "${data_dir}/game/graphics/opengl_renderer/"
cp -R "${project_root}/game/assets/fonts"    "${data_dir}/game/assets/"
cp -R "${project_root}/game/assets/${game}"  "${data_dir}/game/assets/"
cp    "${project_root}/game/assets/sdl_controller_db.txt" "${data_dir}/game/assets/"
cp -R "${project_root}/goal_src/user"        "${data_dir}/goal_src/"

# The compiled game data. get_game_output_dir() appends "-arm64" on Apple
# Silicon, so the directory name inside the bundle must keep that suffix.
cp -R "${out_tree}" "${data_dir}/out/${game}-arm64"

# gk writes log/<timestamp>.log and imgui.ini through get_file_path(), i.e.
# INSIDE the data dir. Any file created inside a signed .app invalidates its
# seal ("file added: ..."), and that happens on the very first run no matter
# where in the bundle data/ lives — Contents/Resources is sealed too.
#
# These are therefore redirected out of the bundle — but the redirect is created
# by the LAUNCHER at runtime, never here. Baking a symlink to
# /Users/<builder>/Library/... into the bundle makes it dangle on every other
# Mac: logging then fails with "File exists" on the broken link and the runtime
# exits before reaching the title screen. The launcher resolves $HOME on the
# machine actually running the app.
#
# Resolve the REAL user's home: under `sudo` (needed to write /Applications)
# $HOME is /var/root, which would seed these links from root's home.
if [[ -n "${SUDO_USER:-}" ]]; then
  real_home="$(dscl . -read "/Users/${SUDO_USER}" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
  : "${real_home:=/Users/${SUDO_USER}}"
else
  real_home="${HOME}"
fi

# The links are created HERE so they exist when the bundle is signed (an entry
# that appears later would be "file added: ..." and break the seal). Their
# target is this machine's home, which is wrong on any other Mac — so the
# launcher re-points them at run time using the running user's $HOME. Rewriting
# a symlink in place does NOT invalidate the signature; only adding or removing
# an entry does. Both halves are required: sign-time creation for the seal,
# run-time retargeting for portability.
support_dir="${real_home}/Library/Application Support/OpenGOAL/${game}"
mkdir -p "${support_dir}/log"
[ -e "${support_dir}/imgui.ini" ] || : > "${support_dir}/imgui.ini"
if [[ -n "${SUDO_USER:-}" ]]; then
  chown -R "${SUDO_USER}" "${real_home}/Library/Application Support/OpenGOAL" 2>/dev/null || true
fi
ln -sfn "${support_dir}/log"        "${data_dir}/log"
ln -sfn "${support_dir}/imgui.ini"  "${data_dir}/imgui.ini"

# --- launcher ----------------------------------------------------------------
# CFBundleExecutable. Finder starts apps with cwd=/, so set it explicitly and
# exec the real binary with the retail flag set (no -v / -debug).
# gk writes log/ and imgui.ini via get_file_path(), i.e. relative to
# get_jak_project_dir() — the data dir, NOT the cwd. If data/ sits inside
# Contents/MacOS it is treated as sealed code, so the first run invalidates the
# signature ("a sealed resource is missing or invalid"). Keeping data/ in
# Contents/Resources and pointing at it with --proj-path avoids that: Resources
# is sealed as data, and the runtime-written files live under a directory the
# launcher creates. --proj-path takes precedence over the beside-the-executable
# data/ probe in setup_project_path().
cat > "${macos_dir}/launcher" <<'LAUNCHER'
#!/bin/bash
here="$(cd "$(dirname "$0")" && pwd)"
data="$(cd "${here}/../Resources/data" && pwd)"

# gk writes log/ and imgui.ini inside the data dir (get_file_path resolves them
# against the project dir, not the cwd, and no CLI flag relocates the log dir).
# Writing into a signed .app invalidates its seal, so both are redirected into
# Application Support. This must happen HERE, at run time, using the running
# machine's $HOME — a symlink baked in at package time points at the build
# machine's home directory and dangles everywhere else, which makes logging fail
# with "File exists" and the app quit before the title screen.
support="${HOME}/Library/Application Support/OpenGOAL/__GAME__"
mkdir -p "${support}/log"
[ -e "${support}/imgui.ini" ] || : > "${support}/imgui.ini"
ln -sfn "${support}/log"       "${data}/log"       2>/dev/null || true
ln -sfn "${support}/imgui.ini" "${data}/imgui.ini" 2>/dev/null || true

exec "${here}/gk" --proj-path "${data}" --game __GAME__ -- -boot -fakeiso
LAUNCHER
sed -i '' "s/__GAME__/${game}/" "${macos_dir}/launcher"
chmod +x "${macos_dir}/launcher"

# --- Info.plist --------------------------------------------------------------
# CFBundleIdentifier also clears gk's "No bundle id found" startup message.
cat > "${contents}/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>${app_name}</string>
  <key>CFBundleDisplayName</key><string>${app_name}</string>
  <key>CFBundleIdentifier</key><string>${bundle_id}</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
PLIST

# --- icon (optional) ---------------------------------------------------------
# The game ships its own 256px app icon; no extra art needed.
icon_src="${project_root}/game/assets/${game}/app256.png"
if [[ -f "${icon_src}" ]] && command -v iconutil >/dev/null 2>&1; then
  work="$(mktemp -d)"
  iconset="${work}/AppIcon.iconset"
  mkdir -p "${iconset}"
  # app256.png is 256x256, so anything above that would be an upscale. Emit only
  # the variants the source can actually fill; macOS scales the rest down itself,
  # which looks better than a blurry synthetic 1024.
  src_px="$(sips -g pixelWidth "${icon_src}" 2>/dev/null | awk '/pixelWidth/{print $2}')"
  : "${src_px:=256}"
  for size in 16 32 128 256 512; do
    [[ ${size} -le ${src_px} ]] && \
      sips -z ${size} ${size} "${icon_src}" --out "${iconset}/icon_${size}x${size}.png" >/dev/null 2>&1
    [[ $((size*2)) -le ${src_px} ]] && \
      sips -z $((size*2)) $((size*2)) "${icon_src}" --out "${iconset}/icon_${size}x${size}@2x.png" >/dev/null 2>&1
  done
  : # keep the conditional chain from tripping set -e
  iconutil -c icns "${iconset}" -o "${contents}/Resources/AppIcon.icns" 2>/dev/null || true
  rm -rf "${work}"
fi

# --- verify self-containment -------------------------------------------------
# This gate exists because the first bundle built by this script LOOKED correct
# (it ran, it was signed) while silently resolving every dylib out of the repo's
# build/ tree. It would have broken the moment the checkout moved. Fail loudly
# rather than ship that again.
echo "    verifying self-containment"
leaks=0
if [[ ${bundle_deps} -eq 1 ]]; then
  while IFS= read -r bin; do
    [[ -n "${bin}" ]] || continue
    # (a) no load command may name the checkout
    if otool -l "${bin}" 2>/dev/null | grep -Fq "${project_root}"; then
      echo "error: ${bin##*/} still references ${project_root}" >&2
      leaks=$((leaks+1))
    fi
    # (b) every non-system dependency must be @rpath/@loader_path, and the file
    #     it names must actually be in Frameworks. An absolute Homebrew path
    #     here is the real-world failure: libcurl referenced
    #     /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib, which does not
    #     exist on a Mac without that exact Homebrew install, so the app died
    #     in dyld with no message.
    while IFS= read -r dep; do
      [[ -n "${dep}" ]] || continue
      case "${dep}" in
        /System/*|/usr/lib/*|/usr/lib) continue ;;
        @rpath/*|@loader_path/*|@executable_path/*)
          depname="${dep##*/}"
          if [[ ! -f "${frameworks_dir}/${depname}" ]]; then
            echo "error: ${bin##*/} needs ${depname}, which is not in Frameworks/" >&2
            leaks=$((leaks+1))
          fi
          ;;
        *)
          echo "error: ${bin##*/} has an absolute dependency: ${dep}" >&2
          leaks=$((leaks+1))
          ;;
      esac
    done < <(otool -L "${bin}" 2>/dev/null | tail -n +2 | sed -E 's/^[[:space:]]+([^[:space:]]+).*/\1/')
  done < <(printf '%s\n' "${macos_dir}/gk"; find "${frameworks_dir}" -name '*.dylib' 2>/dev/null)
  if [[ ${leaks} -gt 0 ]]; then
    echo "error: bundle is NOT portable (${leaks} problems above)" >&2
    exit 3
  fi
  echo "      $(find "${frameworks_dir}" -name '*.dylib' | wc -l | tr -d ' ') dylibs, dependency closure complete"
fi
# data/ must hold real files, not links back into the checkout. The only two
# permitted links are the deliberate log/ and imgui.ini redirects created above,
# which point into Application Support to keep runtime writes out of the bundle.
stray_links="$(find "${data_dir}" -type l 2>/dev/null \
  | grep -v -e "^${data_dir}/log$" -e "^${data_dir}/imgui.ini$" || true)"
if [[ -n "${stray_links}" ]]; then
  echo "error: unexpected symlinks in data/:" >&2
  printf '  %s\n' ${stray_links} >&2
  exit 3
fi

# --- sign --------------------------------------------------------------------
# Sign AFTER all install_name_tool edits and the icon, or the signature is stale.
# Signing a multi-GB bundle can fail transiently while the filesystem is still
# settling; retry, and NEVER swallow the error output — an earlier version of
# this script hid both the codesign failure and the verification failure, and
# reported success on a completely unsigned bundle.
echo "    signing (ad-hoc)"
# Deepest-first: signing a nested binary invalidates any enclosing signature, so
# each dylib is signed before the bundle that contains them.
if [[ ${bundle_deps} -eq 1 ]]; then
  while IFS= read -r lib; do
    [[ -n "${lib}" ]] || continue
    if ! codesign --force -s - "${lib}" 2>/dev/null; then
      echo "error: failed to sign ${lib##*/}" >&2
      exit 3
    fi
  done < <(find "${frameworks_dir}" -name '*.dylib' 2>/dev/null)
fi
# gk is a JIT: common/jit_memory.h maps executable pages with MAP_JIT, and the
# build hard-errors without it. A signed binary may only use MAP_JIT if it
# carries com.apple.security.cs.allow-jit. Omitting the entitlement produces a
# bundle that runs on the machine that signed it but is KILLED AT LAUNCH
# elsewhere — it bounces in the Dock and exits with no crash report, because the
# refusal happens before any of our code runs. CMake only wires this file into
# the Xcode generator, so a Ninja build never gets it and the packaging step
# must apply it itself.
entitlements="${project_root}/game/macos/jak-jit.entitlements"
if [[ ! -f "${entitlements}" ]]; then
  echo "error: missing ${entitlements}; the bundle would not launch on another Mac" >&2
  exit 3
fi

# Order matters and --deep must NOT be used here. The entitlement belongs on the
# Mach-O binary, not the bundle (CFBundleExecutable is a shell script, and
# codesign cannot attach entitlements to a script). A --deep pass on the bundle
# re-signs gk and silently drops the entitlement, while signing gk AFTER the
# bundle invalidates the outer seal. So: dylibs, then gk with entitlements, then
# the bundle alone.
if ! codesign --force --entitlements "${entitlements}" -s - "${macos_dir}/gk" 2>/dev/null; then
  echo "error: failed to sign gk with the JIT entitlement" >&2
  exit 3
fi

sign_ok=0
for attempt in 1 2 3; do
  sign_out="$(codesign --force -s - "${app}" 2>&1)" && sign_rc=0 || sign_rc=$?
  [[ -n "${sign_out}" ]] && printf '      %s\n' "${sign_out}"
  if [[ ${sign_rc} -eq 0 ]]; then
    verify_out="$(codesign -v "${app}" 2>&1)" && verify_rc=0 || verify_rc=$?
    [[ -n "${verify_out}" ]] && printf '      %s\n' "${verify_out}"
    if [[ ${verify_rc} -eq 0 ]]; then
      sign_ok=1
      break
    fi
  fi
  echo "      signing attempt ${attempt} failed; retrying"
  sleep 2
done
if [[ ${sign_ok} -ne 1 ]]; then
  echo "error: could not produce a valid signature for ${app}" >&2
  exit 3
fi

# The JIT entitlement must actually be present in the signature. Without it the
# app launches here and dies silently on every other Mac, which is the hardest
# class of bug to notice from the build machine.
# Verify on gk itself, not the bundle: the bundle's CFBundleExecutable is the
# launcher script and never carries entitlements.
if ! codesign -d --entitlements - --xml "${macos_dir}/gk" 2>/dev/null | grep -q "allow-jit"; then
  echo "error: gk is missing com.apple.security.cs.allow-jit" >&2
  echo "       the app would bounce in the Dock and quit on any other Mac" >&2
  exit 3
fi
echo "      JIT entitlement present on gk"

echo "==> ${app}"
echo "    size: $(du -sh "${app}" | cut -f1)"
codesign -dv "${app}" 2>&1 | grep -E "Signature|Identifier" | sed 's/^/    /' || true
