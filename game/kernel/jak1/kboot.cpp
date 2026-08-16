/*!
 * @file kboot.cpp
 * GOAL Boot.  Contains the "main" function to launch GOAL runtime
 * DONE!
 */

#include "kboot.h"

#include <chrono>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/common/game_common_types.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/ksocket.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kmachine.h"
#include "game/sce/libscf.h"

using namespace ee;

namespace jak1 {
VideoMode BootVideoMode;

void kboot_init_globals() {}

/*!
 * Launch the GOAL Kernel (EE).
 * DONE!
 * See InitParms for launch argument details.
 * @param argc : argument count
 * @param argv : argument list
 * @return 0 on success, otherwise failure.
 *
 * CHANGES:
 * Added InitParms call to handle command line arguments
 * Removed hard-coded debug mode disable
 * Renamed from `main` to `goal_main`
 * Add call to sceDeci2Reset when GOAL shuts down.
 */
s32 goal_main(int argc, const char* const* argv) {
  // Initialize global variables based on command line parameters
  // This call is not present in the retail version of the game
  // but the function is, and it likely goes here.
  InitParms(argc, argv);

  // Initialize CRC32 table for string hashing
  init_crc();

  // NTSC V1, NTSC v2, PAL CD Demo, PAL Retail
  // Set up game configurations
  masterConfig.aspect = (u16)sceScfGetAspect();
  masterConfig.language = (u16)sceScfGetLanguage();
  masterConfig.inactive_timeout = 0;  // demo thing
  masterConfig.timeout = 0;           // demo thing
  masterConfig.volume = 100;

  // Set up language configuration
  if (masterConfig.language == SCE_SPANISH_LANGUAGE) {
    masterConfig.language = (u16)Language::Spanish;
  } else if (masterConfig.language == SCE_FRENCH_LANGUAGE) {
    masterConfig.language = (u16)Language::French;
  } else if (masterConfig.language == SCE_GERMAN_LANGUAGE) {
    masterConfig.language = (u16)Language::German;
  } else if (masterConfig.language == SCE_ITALIAN_LANGUAGE) {
    masterConfig.language = (u16)Language::Italian;
  } else if (masterConfig.language == SCE_JAPANESE_LANGUAGE) {
    // Note: this case was added so it is easier to test Japanese fonts.
    masterConfig.language = (u16)Language::Japanese;
  } else {
    // pick english by default, if language is not supported.
    masterConfig.language = (u16)Language::English;
  }

  // Set up aspect ratio override in demo
  if (!strcmp(DebugBootMessage, "demo") || !strcmp(DebugBootMessage, "demo-shared")) {
    masterConfig.aspect = SCE_ASPECT_FULL;
  }

  // In retail game, disable debugging modes, and force on DiskBoot
  // MasterDebug = 0;
  // DiskBoot = 1;
  // DebugSegment = 0;

  // Launch GOAL!
  if (InitMachine() >= 0) {    // init kernel
    KernelCheckAndDispatch();  // run kernel
    ShutdownMachine();         // kernel died, we should too.
  } else {
    fprintf(stderr, "InitMachine failed\n");
    exit(1);
  }

  return 0;
}

/*!
 * Main loop to dispatch the GOAL kernel.
 */
void KernelCheckAndDispatch() {
#if defined(__aarch64__)
  // AArch64 faults (EXC_ARM_SP_ALIGN) unless SP is 16-byte aligned.
  u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - 16;
#else
  u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - 8;
#endif

  // SYMWATCH=<hex slot offsets, comma separated>: print the value at each slot
  // every N dispatches. Offsets must come from SYMDUMP (the linker's own
  // name->slot mapping) — lldb name lookup is unsound here, see PORTING-NOTES.md.
  std::vector<u32> symwatch;
  if (const char* w = getenv("SYMWATCH")) {
    const char* p = w;
    while (*p) {
      symwatch.push_back((u32)strtoul(p, nullptr, 0));
      const char* c = strchr(p, ',');
      if (!c) {
        break;
      }
      p = c + 1;
    }
  }
  u64 symwatch_tick = 0;

  // GK_FREEZE_AT_DISPATCH=<n>: from dispatch n onward, hold the game in pause mode.
  //
  // This exists to make an automated screenshot comparable across graphics backends.
  // A frame number alone pins WHEN a capture happens, not WHAT is on screen (see
  // screenshot.h): loading is paced by wall-clock I/O, so two runs reach the same
  // frame index in different states, and even at identical game state the actors are
  // at different points in their animation. Comparing there measures animation phase
  // rather than rendering.
  //
  // Pausing fixes that at the source. *master-mode* = 'pause makes paused? true
  // (main.gc), and display-frame-start gates every game clock on (not (paused?)) --
  // base-frame-counter, part-frame-counter, integral-frame-counter and the rest all
  // stop (drawable.gc). Since current-time IS base-frame-counter (display-h.gc),
  // animation, particles, texture scroll and time-of-day all stop with it, while the
  // renderer keeps drawing live frames from a fresh DMA chain each time. That is
  // exactly the requirement: a still scene, still being rendered.
  //
  // Written every dispatch rather than once, because determine-pause-mode (drawable.gc)
  // calls toggle-pause on controller input and on *pause-lock*, and a one-shot write
  // would be silently undone. *cheat-mode* = 'camera suppresses the red PAUSE string
  // that pause otherwise draws across the middle of the screen (main.gc), which would
  // occlude the region being compared.
  //
  // Debug/automation only; does nothing unless the variable is set.
  const char* freeze_env = getenv("GK_FREEZE_AT_DISPATCH");
  const u64 freeze_at = freeze_env ? strtoull(freeze_env, nullptr, 0) : 0;
  u64 dispatch_count = 0;
  bool freeze_announced = false;
  if (freeze_at) {
    lg::info("GK_FREEZE_AT_DISPATCH: will hold pause mode from dispatch {}", freeze_at);
  }

  while (MasterExit == RuntimeExitStatus::RUNNING) {
    if (freeze_at && ++dispatch_count >= freeze_at) {
      // Go through set-master-mode rather than writing *master-mode* directly.
      //
      // Writing the symbol alone does freeze the clocks -- paused? reads it, and
      // display-frame-start gates base-frame-counter on paused?. But that is only half
      // the freeze, and the missing half is visible: set-master-mode also sets the
      // pause bit in *setting-control*'s process-mask, which apply-settings copies into
      // *kernel-context* prevent-from-run, which is what makes execute-process-tree skip
      // the actors themselves (gkernel.gc). Without it, any process whose :trans
      // animates from its own state rather than from the frame counter keeps running --
      // measured on jak1's title vista, where the windmill sails, the birds and the
      // ocean surface all kept moving between two otherwise-identical captures while
      // the camera and the rest of the scene held still.
      //
      // *cheat-mode* = 'camera suppresses the red PAUSE string that pause draws across
      // the middle of the screen (main.gc), which would occlude the compared region.
      intern_from_c("*cheat-mode*")->value = intern_from_c("camera")->value;
      const auto set_master_mode = Ptr<Function>(*(intern_from_c("set-master-mode")).cast<u32>());
      call_goal(set_master_mode, intern_from_c("pause")->value, 0, 0, s7.offset, g_ee_main_mem);
      if (!freeze_announced) {
        freeze_announced = true;
        lg::info("GK_FREEZE_AT_DISPATCH: holding pause mode from dispatch {}", dispatch_count);
      }
    }

    if (!symwatch.empty() && (symwatch_tick++ % 120) == 0) {
      fprintf(stderr, "SYMWATCH tick %llu:", (unsigned long long)symwatch_tick);
      for (u32 off : symwatch) {
        fprintf(stderr, " [0x%x]=0x%x", off, *Ptr<u32>(off));
      }
      fprintf(stderr, "\n");
    }

    // try to get a message from the listener, and process it if needed
    Ptr<char> new_message = WaitForMessageAndAck();
    if (new_message.offset) {
      ProcessListenerMessage(new_message);
    }

    // remember the old listener function
    auto old_listener = ListenerFunction->value;
    // dispatch the kernel
    //(**kernel_dispatcher)();

    Timer kernel_dispatch_timer;
    if (MasterUseKernel) {
      // use the GOAL kernel.
      call_goal_on_stack(Ptr<Function>(kernel_dispatcher->value), goal_stack, s7.offset,
                         g_ee_main_mem);
    } else {
      // use a hack to just run the listener function if there's no GOAL kernel.
      if (ListenerFunction->value != s7.offset) {
        auto result = call_goal_on_stack(Ptr<Function>(ListenerFunction->value), goal_stack,
                                         s7.offset, g_ee_main_mem);
#ifdef __linux__
        cprintf("%ld\n", result);
#else
        cprintf("%lld\n", result);
#endif
        ListenerFunction->value = s7.offset;
      }
    }

    auto time_ms = kernel_dispatch_timer.getMs();
    if (time_ms > 50) {
      lg::print("Kernel dispatch time: {:.3f} ms\n", time_ms);
    }

    ClearPending();

    // if the listener function changed, it means the kernel ran it, so we should notify compiler.
    if (MasterDebug && ListenerFunction->value != old_listener) {
      SendAck();
    }

    if (time_ms < 4) {
      std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
  }
}

/*!
 * Stop running the GOAL Kernel.
 * DONE, EXACT
 */
void KernelShutdown() {
  MasterExit = RuntimeExitStatus::EXIT;  // GOAL Kernel Dispatch loop will stop now.
}
}  // namespace jak1
