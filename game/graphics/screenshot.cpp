#include "game/graphics/screenshot.h"

#include <cstdlib>
#include <string>

#include "common/log/log.h"

/*!
 * @file screenshot.cpp
 * This file contains a basic interface for the screen shot system to make it easier to share with
 * the GOAL kernel.
 */

ScreenShotSettings s_default_screen_shot_settings = {1920, 1080, 8, "screenshot"};

bool g_want_screenshot = false;
ScreenShotSettings* g_screen_shot_settings = &s_default_screen_shot_settings;

void register_screen_shot_settings(ScreenShotSettings* settings) {
  g_screen_shot_settings = settings;
}

const char* get_screen_shot_name() {
  return g_screen_shot_settings->name;
}

namespace {
// Parsed once from GK_SCREENSHOT_AT_FRAME. target_frame <= 0 means "never".
struct HostTrigger {
  int target_frame = 0;
  std::string path;
};

const HostTrigger& host_trigger() {
  static const HostTrigger t = []() {
    HostTrigger out;
    const char* v = getenv("GK_SCREENSHOT_AT_FRAME");
    if (!v || !v[0]) {
      return out;
    }
    std::string s(v);
    auto comma = s.find(',');
    std::string frame_part = comma == std::string::npos ? s : s.substr(0, comma);
    if (comma != std::string::npos) {
      out.path = s.substr(comma + 1);
    }
    try {
      out.target_frame = std::stoi(frame_part);
    } catch (const std::exception&) {
      lg::error("GK_SCREENSHOT_AT_FRAME: could not parse a frame number from \"{}\"; ignoring",
                frame_part);
      out.target_frame = 0;
      return out;
    }
    if (out.target_frame <= 0) {
      lg::error("GK_SCREENSHOT_AT_FRAME: frame must be >= 1, got {}; ignoring", out.target_frame);
      out.target_frame = 0;
      return out;
    }
    lg::info("GK_SCREENSHOT_AT_FRAME: will capture frame {}{}", out.target_frame,
             out.path.empty() ? "" : (" to " + out.path));
    return out;
  }();
  return t;
}

std::string g_host_trigger_path;
}  // namespace

void screenshot_check_host_trigger() {
  const auto& trigger = host_trigger();
  if (trigger.target_frame <= 0) {
    return;
  }
  // Counts every frame this is called on, so the frame number means the same thing
  // regardless of what the game is doing.
  static int frame = 0;
  ++frame;
  if (frame == trigger.target_frame) {
    g_host_trigger_path = trigger.path;
    g_want_screenshot = true;
    lg::info("GK_SCREENSHOT_AT_FRAME: requesting capture on frame {}", frame);
  }
}

const char* get_host_trigger_screenshot_path() {
  return g_host_trigger_path.c_str();
}
