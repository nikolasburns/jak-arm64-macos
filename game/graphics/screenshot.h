#pragma once

#include <memory>

#include "common/common_types.h"

#include "game/kernel/common/kscheme.h"

/*!
 * @file screenshot.h
 * This file contains a basic interface for the screen shot system to make it easier to share with
 * the GOAL kernel.
 */

// this must match the structure in capture-pc.gc (if present)
struct ScreenShotSettings {
  s32 width;
  s32 height;
  s32 msaa;
  char name[244];
};

extern ScreenShotSettings* g_screen_shot_settings;
extern bool g_want_screenshot;

void register_screen_shot_settings(ScreenShotSettings* settings);
const char* get_screen_shot_name();

/*!
 * Host-side screenshot trigger, for capturing reference images without a human at
 * the keyboard.
 *
 * The in-game paths (the F2 hotkey, the ImGui button, and GOAL's capture-pc) are all
 * driven by a person or by the game's own script, so an automated A/B capture -- for
 * example comparing two graphics backends at the same point in a boot -- had no way
 * to ask for a frame. This provides one:
 *
 *   GK_SCREENSHOT_AT_FRAME=<frame>[,<path>]
 *
 * On the given rendered frame (1-based), request a screenshot by setting the same
 * g_want_screenshot flag the GOAL path sets, so the capture takes the identical
 * internal-resolution route and no second code path exists to diverge. If <path> is
 * given the image is written there verbatim; otherwise the usual timestamped
 * screenshot filename is used.
 *
 * Debug/automation tool. Does nothing unless the variable is set.
 *
 * DETERMINISM, measured -- read this before using captures as a reference corpus.
 * A frame number pins WHEN the capture happens, not WHAT is on screen. Two boots
 * reach different game state on the same frame index, because loading is paced by
 * wall-clock I/O rather than by frame count. Measured on jak1 -boot:
 *
 *   frame 60  -- byte-identical across two runs, but the frame is entirely BLACK,
 *                so the match proves nothing about the renderer.
 *   frame 400 -- both runs land on the Naughty Dog logo scene with Daxter rendered
 *                correctly, but at DIFFERENT points in his animation, so the PNGs
 *                differ.
 *
 * A capture is therefore only comparable across backends when the scene it lands on
 * is static. Comparing two backends at an animating moment measures animation phase,
 * not rendering. Pick a still scene, verify two same-backend captures match AND that
 * the frame is not blank, and only then compare across backends.
 */
void screenshot_check_host_trigger();

// The path the host trigger asked for, or empty if it did not ask for a specific
// one. Only meaningful on the frame the trigger fires.
const char* get_host_trigger_screenshot_path();
