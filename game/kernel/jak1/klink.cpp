#include "klink.h"

#include "common/jit_memory.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/fileio.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/mips2c_table.h"

#include "fmt/format.h"

static constexpr bool link_debug_printfs = false;
/*!
 * Make progress on linking.
 */
// SYMDUMP support: name of the object currently being linked.
static const char* g_symdump_obj = "?";

uint32_t link_control::jak1_work() {
  auto old_debug_segment = DebugSegment;
  if (m_keep_debug) {
    DebugSegment = s7.offset + true_symbol_offset(g_game_version);
  }

  // set type tag of link block
  *((m_link_block_ptr - 4).cast<u32>()) = *((s7 + jak1_symbols::FIX_SYM_LINK_BLOCK).cast<u32>());

  uint32_t rv;

  if (m_version == 3) {
    ASSERT(m_opengoal);
    rv = jak1_work_v3();
  } else if (m_version == 2 || m_version == 4) {
    ASSERT(!m_opengoal);
    rv = jak1_work_v2();
  } else {
    ASSERT_MSG(false, fmt::format("UNHANDLED OBJECT FILE VERSION {} IN WORK!", m_version));
    return 0;
  }

  DebugSegment = old_debug_segment;
  return rv;
}
namespace {
/*!
 * Link a single relative offset (used for RIP)
 */
uint32_t cross_seg_dist_link_v3(Ptr<uint8_t> link,
                                ObjectFileHeader* ofh,
                                int current_seg,
                                int size) {
  // target seg, dist into mine, dist into target, patch loc in mine
  uint8_t target_seg = *link;
  ASSERT(target_seg < ofh->segment_count);

  uint32_t* link_data = (link + 1).cast<uint32_t>().c();
  int32_t mine = link_data[0] + ofh->code_infos[current_seg].offset;
  int32_t tgt = link_data[1] + ofh->code_infos[target_seg].offset;
  int32_t diff = tgt - mine;
  uint32_t offset_of_patch = link_data[2] + ofh->code_infos[current_seg].offset;

  if (!ofh->code_infos[target_seg].offset) {
    // we want to address GOAL 0. In the case where this is a rip-relative load or store, this
    // will crash, which is what we want. If it's an lea and just getting an address, this will get
    // us a nullptr. If you do a method-set! with a null pointer it does nothing, so it's safe to
    // method-set! to things that are in unloaded segments and it'll just keep the old method.
    diff = -mine;
  }
  // printf("link object in seg %d diff %d at %d (%d + %d)\n", target_seg, diff, offset_of_patch,
  // link_data[2], ofh->code_infos[current_seg].offset);

  // both 32-bit and 64-bit pointer links are supported, though 64-bit ones are no longer in use.
  // we still support it just in case we want to run ancient code.
  if (size == 4) {
    *Ptr<int32_t>(offset_of_patch).c() = diff;
  } else if (size == 8) {
    *Ptr<int64_t>(offset_of_patch).c() = diff;
  } else {
    ASSERT(false);
  }

  return 1 + 3 * 4;
}

uint32_t ptr_link_v3(Ptr<u8> link, ObjectFileHeader* ofh, int current_seg) {
  auto* link_data = link.cast<u32>().c();
  u32 patch_loc = link_data[0] + ofh->code_infos[current_seg].offset;
  u32 patch_value = link_data[1] + ofh->code_infos[current_seg].offset;
  *Ptr<u32>(patch_loc).c() = patch_value;
  return 8;
}

/*!
 * Link type pointers for a single type in "v3 equivalent" link data
 * Returns a pointer to the link table data after the typelinking data.
 */
uint32_t typelink_v3(Ptr<uint8_t> link, Ptr<uint8_t> data) {
  // get the name of the type
  uint32_t seek = 0;
  char sym_name[256];
  while (link.c()[seek]) {
    sym_name[seek] = link.c()[seek];
    seek++;
    ASSERT(seek < 256);
  }
  sym_name[seek] = 0;
  seek++;

  // determine the number of methods
  uint8_t method_count = link.c()[seek++];

  // intern the GOAL type, creating the vtable if it doesn't exist.
  auto type_ptr = jak1::intern_type_from_c(sym_name, method_count);

  // prepare to read the locations of the type pointers
  Ptr<uint32_t> offsets = link.cast<uint32_t>() + seek;
  uint32_t offset_count = *offsets;
  offsets = offsets + 4;
  seek += 4;

  // write the type pointers into memory
  for (uint32_t i = 0; i < offset_count; i++) {
    *(data + offsets.c()[i]).cast<int32_t>() = type_ptr.offset;
    seek += 4;
  }

  return seek;
}

/*!
 * Link symbols (both offsets and pointers) in "v3 equivalent" link data.
 * Returns a pointer to the link table data after the linking data for this symbol.
 */
uint32_t symlink_v3(Ptr<uint8_t> link, Ptr<uint8_t> data) {
  // get the symbol name
  uint32_t seek = 0;
  char sym_name[256];
  while (link.c()[seek]) {
    sym_name[seek] = link.c()[seek];
    seek++;
    ASSERT(seek < 256);
  }
  sym_name[seek] = 0;
  seek++;

  // intern
  auto sym = jak1::intern_from_c(sym_name);
  int32_t sym_offset = sym.cast<u32>() - s7;
  uint32_t sym_addr = sym.cast<u32>().offset;

  // SYMDUMP=<comma-separated names>: print the authoritative slot offset for
  // these symbols as the linker resolves them. The linker runs in real runtime
  // context, so this is the only reliable name->slot mapping available (lldb
  // name lookup is unsound here; see PORTING-NOTES.md).
  if (const char* want = getenv("SYMDUMP")) {
    if (strstr(want, sym_name)) {
      fprintf(stderr, "SYMDUMP obj=%-16s %-28s slot=0x%x s7rel=%d value=0x%x\n",
              g_symdump_obj, sym_name, sym_addr, sym_offset, *(sym.cast<u32>()));
    }
  }

  // prepare to read locations of symbol links
  Ptr<uint32_t> offsets = link.cast<uint32_t>() + seek;
  uint32_t offset_count = *offsets;
  offsets = offsets + 4;
  seek += 4;

  for (uint32_t i = 0; i < offset_count; i++) {
    uint32_t offset = offsets.c()[i];
    seek += 4;
    auto data_ptr = (data + offset).cast<int32_t>();

    if (*data_ptr == -1) {
      // a "-1" indicates that we should store the address.
      *(data + offset).cast<int32_t>() = sym_addr;
    } else {
      // otherwise store the offset to st.  Eventually this should become an s16 instead.
      *(data + offset).cast<int32_t>() = sym_offset;
    }
  }

  return seek;
}

}  // namespace
/*!
 * Run the linker. For now, all linking is done in two runs.  If this turns out to be too slow,
 * this should be modified to do incremental linking over multiple runs.
 */
// Debugger anchor, deliberately not static and not inlined so a breakpoint can
// be set on it by name. Called once, at the moment the watched string is known
// to be valid. Does nothing on its own.
extern "C" __attribute__((noinline)) void symguard_arm_watch(u32 str_offset) {
  asm volatile("" : : "r"(str_offset) : "memory");
}

// Debugger anchor for watching a *part-group-id-table* entry. Called once, with
// the table's GOAL offset in the first argument, at a moment the entry is known
// valid. Set a breakpoint here and arm a hardware watchpoint on
// base + tbl + 12 + index*4. Does nothing on its own.
extern "C" __attribute__((noinline)) void partcensus_arm_watch(u32 table_offset) {
  asm volatile("" : : "r"(table_offset) : "memory");
}

// PARTCENSUS: census of *part-group-id-table*, to answer "is slot 656 (and which
// others) empty, and are the empties clustered or scattered?".
// Set PARTCENSUS=<object name> to dump after that object links (e.g.
// PARTCENSUS=game-save), or PARTCENSUS=1 to dump after every object that changes
// the populated count. Prints slot 656 explicitly every time.
// The table is `(new 'global 'boxed-array sparticle-launch-group 1024)`.
// array layout (TypeSystem.cpp:1249): length +0, allocated-length +4,
// content-type +8, data +12. Entries are 4-byte GOAL pointers; empty == s7.
static void partcensus_check(const char* after_object) {
  const char* want = getenv("PARTCENSUS");
  if (!want) {
    return;
  }
  // Resolve the table by SYMBOL, then sanity-check the array header before
  // trusting it -- per PORTING-NOTES.md, a name lookup that "succeeds" is not enough.
  auto sym = jak1::find_symbol_from_c("*part-group-id-table*");
  if (!sym.offset) {
    lg::error("PARTCENSUS: symbol *part-group-id-table* not found after {}", after_object);
    return;
  }
  u32 tbl = sym->value;
  if (!tbl) {
    lg::error("PARTCENSUS: *part-group-id-table* is 0 after {}", after_object);
    return;
  }
  s32 length = *Ptr<s32>(tbl + 0).c();
  s32 alloc = *Ptr<s32>(tbl + 4).c();
  if (length <= 0 || length > 4096 || alloc <= 0 || alloc > 4096) {
    lg::error("PARTCENSUS: bad array header after {} (len={} alloc={}) -- not trusting it",
              after_object, length, alloc);
    return;
  }
  u32 s7v = s7.offset;
  u32* data = Ptr<u32>(tbl + 12).c();
  int populated = 0;
  for (s32 i = 0; i < length; i++) {
    if (data[i] != 0 && data[i] != s7v) {
      populated++;
    }
  }
  static int last_populated = -1;
  bool named = (strcmp(want, "1") != 0);
  if (named && strcmp(want, after_object) != 0) {
    if (populated == last_populated) {
      return;
    }
  }
  if (!named && populated == last_populated) {
    return;
  }
  last_populated = populated;

  u32 e656 = (656 < length) ? data[656] : 0;
  lg::warn("PARTCENSUS after {}: populated {}/{} (alloc {}), slot656 = {:#x} [{}]", after_object,
           populated, length, alloc, e656,
           (e656 == 0 || e656 == s7v) ? "EMPTY" : "ok");

  // Arm point: once slot 656 is genuinely populated, hand the table offset to the
  // debugger anchor so a hardware watchpoint can be placed on that entry.
  // PARTWATCH=1 enables; fires once.
  static bool armed = false;
  if (!armed && getenv("PARTWATCH") && e656 != 0 && e656 != s7v) {
    armed = true;
    lg::warn("PARTCENSUS: arming watch anchor, table offset {:#x}, entry addr = base+{:#x}", tbl,
             tbl + 12 + 656 * 4);
    partcensus_arm_watch(tbl);
  }

  // Cluster analysis: contiguous runs of EMPTY slots. A single long run means one
  // object's top-level never completed; scattered singletons mean per-store loss.
  int runs = 0, longest = 0, longest_at = -1, cur = 0, cur_at = -1;
  for (s32 i = 0; i < length; i++) {
    bool empty = (data[i] == 0 || data[i] == s7v);
    if (empty) {
      if (cur == 0) {
        cur_at = i;
      }
      cur++;
    } else {
      if (cur > 0) {
        runs++;
        if (cur > longest) {
          longest = cur;
          longest_at = cur_at;
        }
      }
      cur = 0;
    }
  }
  if (cur > 0) {
    runs++;
    if (cur > longest) {
      longest = cur;
      longest_at = cur_at;
    }
  }
  lg::warn("PARTCENSUS   empty-runs={} longest={} @{}", runs, longest, longest_at);
  // Neighbours of 656, to see whether the whole neighbourhood is missing.
  std::string near;
  for (s32 i = 648; i < 665 && i < length; i++) {
    near += fmt::format("{}{}", (data[i] == 0 || data[i] == s7v) ? "." : "#", (i == 656) ? "*" : "");
  }
  lg::warn("PARTCENSUS   slots 648..664: {} (. = empty, * marks 656)", near);
}

// SYMGUARD: watch a known symbol's name string for in-place corruption.
// Set SYMGUARD=1. Prints the object being linked when the bytes first change.
static void symguard_check(const char* when) {
  if (!getenv("SYMGUARD")) {
    return;
  }
  static char last[32];
  static bool have = false;
  static u32 pinned_str = 0;  // pin the ADDRESS: once corrupted, name lookup fails
  Ptr<String> strp;
  if (pinned_str) {
    strp = Ptr<String>(pinned_str);
  } else {
    auto sym = jak1::find_symbol_from_c("*default-dead-pool*");
    if (!sym.offset) {
      return;
    }
    strp = jak1::info(sym)->str;
    if (!strp.offset) {
      return;
    }
    pinned_str = strp.offset;
  }
  const char* d = strp->data();
  if (!have) {
    strncpy(last, d, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0;
    have = true;
    fprintf(stderr, "SYMGUARD baseline at %s: str=0x%x %.24s\n", when, strp.offset, d);
    // Anchor for a debugger: the string is valid RIGHT NOW, so this is the
    // correct moment to arm a hardware watchpoint on its bytes. Breaking on
    // this symbol avoids having to guess a wall-clock moment to attach.
    symguard_arm_watch(strp.offset);
    return;
  }
  if (strncmp(last, d, sizeof(last) - 1) != 0) {
    fprintf(stderr, "SYMGUARD *** CORRUPTED at %s *** (str=0x%x)\n", when, strp.offset);
    // Dump the raw bytes AROUND the string, including the String header
    // (type tag at -4, allocated_length at +0, data at +4) so the corrupt
    // content can be interpreted: ASCII? a GOAL pointer? instruction words?
    const u8* raw = (const u8*)(g_ee_main_mem + strp.offset) - 16;
    for (int row = 0; row < 4; row++) {
      u32 goal_addr = strp.offset - 16 + row * 16;
      fprintf(stderr, "  SYMGUARD %c[0x%x] ", (row == 1 ? '>' : ' '), goal_addr);
      for (int i = 0; i < 16; i++) {
        fprintf(stderr, "%02x ", raw[row * 16 + i]);
      }
      fprintf(stderr, " |");
      for (int i = 0; i < 16; i++) {
        u8 c = raw[row * 16 + i];
        fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
      }
      fprintf(stderr, "|\n");
    }
    // interpret the 8 clobbered bytes as u32 pairs: GOAL ptr? instruction?
    const u32* w = (const u32*)((const u8*)(g_ee_main_mem + strp.offset) + 8);
    fprintf(stderr, "  SYMGUARD clobber words: 0x%08x 0x%08x\n", w[0], w[1]);
    // The repeating pointer in the clobber pattern names the table's owner.
    // Read its type word (offset -4) and then that type's name symbol/string.
    {
      u32 cand = w[1];
      if (cand > 0x1000 && cand < 0x8000000) {
        u32 type_ptr = *(const u32*)(g_ee_main_mem + cand - 4);
        fprintf(stderr, "  SYMGUARD repeated-ptr 0x%x -> type word 0x%x", cand, type_ptr);
        if (type_ptr > 0x1000 && type_ptr < 0x8000000) {
          auto tsym = Ptr<jak1::Type>(type_ptr)->symbol;
          if (tsym.offset) {
            auto tstr = jak1::info(tsym)->str;
            if (tstr.offset) {
              fprintf(stderr, " type=%.32s", tstr->data());
            }
          }
        }
        fprintf(stderr, "\n");
      }
    }
    strncpy(last, d, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0;
  }
}

uint32_t link_control::jak1_work_v3() {
  g_symdump_obj = m_object_name;
  symguard_check(m_object_name);
  ObjectFileHeader* ofh = m_link_block_ptr.cast<ObjectFileHeader>().c();
  if (m_state == 0) {
    // state 0 <- copying data.
    // the actual game does all copying in one shot. I assume this is ok because v3 files are just
    // code and always small.  Large data which takes too long to copy should use v2.

    // loop over segments
    for (s32 seg_id = ofh->segment_count - 1; seg_id >= 0; seg_id--) {
      // link the infos
      ofh->link_infos[seg_id].offset += m_link_block_ptr.offset;
      ofh->code_infos[seg_id].offset += m_object_data.offset;

      if (seg_id == DEBUG_SEGMENT) {
        if (!DebugSegment) {
          // clear code info if we aren't going to copy the debug segment.
          ofh->code_infos[seg_id].offset = 0;
          ofh->code_infos[seg_id].size = 0;
        } else {
          if (ofh->code_infos[seg_id].size == 0) {
            // not actually present
            ofh->code_infos[seg_id].offset = 0;
          } else {
            Ptr<u8> src(ofh->code_infos[seg_id].offset);
            ofh->code_infos[seg_id].offset =
                kmalloc(kdebugheap, ofh->code_infos[seg_id].size, KMALLOC_EXECUTABLE,
                        "debug-segment")
                    .offset;
            if (ofh->code_infos[seg_id].offset == 0) {
              MsgErr("dkernel: unable to malloc %d bytes for debug-segment\n",
                     ofh->code_infos[seg_id].size);
              return 1;
            }
            jak1::ultimate_memcpy(Ptr<u8>(ofh->code_infos[seg_id].offset).c(), src.c(),
                                  ofh->code_infos[seg_id].size);
          }
        }
      } else if (seg_id == MAIN_SEGMENT) {
        if (ofh->code_infos[seg_id].size == 0) {
          ofh->code_infos[seg_id].offset = 0;
        } else {
          Ptr<u8> src(ofh->code_infos[seg_id].offset);
          ofh->code_infos[seg_id].offset =
              kmalloc(m_heap, ofh->code_infos[seg_id].size, KMALLOC_EXECUTABLE, "main-segment")
                  .offset;
          if (ofh->code_infos[seg_id].offset == 0) {
            MsgErr("dkernel: unable to malloc %d bytes for main-segment\n",
                   ofh->code_infos[seg_id].size);
            return 1;
          }
          jak1::ultimate_memcpy(Ptr<u8>(ofh->code_infos[seg_id].offset).c(), src.c(),
                                ofh->code_infos[seg_id].size);
          // TEMP DIAGNOSTIC: map runtime fault addresses back to per-function
          // compile-time offsets (see GOALC_DUMP_FUNC_OFFSETS).
          if (getenv("GK_DUMP_SEGMENTS")) {
            fprintf(stderr, "SEGMAP %s seg=%d base=0x%x size=0x%x\n", m_object_name, seg_id,
                    ofh->code_infos[seg_id].offset, ofh->code_infos[seg_id].size);
          }
        }
      } else if (seg_id == TOP_LEVEL_SEGMENT) {
        if (ofh->code_infos[seg_id].size == 0) {
          ofh->code_infos[seg_id].offset = 0;
        } else {
          Ptr<u8> src(ofh->code_infos[seg_id].offset);
          ofh->code_infos[seg_id].offset =
              kmalloc(m_heap, ofh->code_infos[seg_id].size, KMALLOC_TOP | KMALLOC_EXECUTABLE,
                      "top-level-segment")
                  .offset;
          if (ofh->code_infos[seg_id].offset == 0) {
            MsgErr("dkernel: unable to malloc %d bytes for top-level-segment\n",
                   ofh->code_infos[seg_id].size);
            return 1;
          }
          jak1::ultimate_memcpy(Ptr<u8>(ofh->code_infos[seg_id].offset).c(), src.c(),
                                ofh->code_infos[seg_id].size);
        }
      } else {
        printf("UNHANDLED SEG ID IN WORK V3 STATE 1\n");
      }
    }

    m_state = 1;
    m_segment_process = 0;
    return 0;
  } else if (m_state == 1) {
    // state 1: linking. For now all links are done at once. This is probably going to be fine on a
    // modern computer.  But the game broke this into multiple steps.
    if (m_segment_process < ofh->segment_count) {
      if (ofh->code_infos[m_segment_process].offset) {
        Ptr<u8> lp(ofh->link_infos[m_segment_process].offset);

        while (*lp) {
          switch (*lp) {
            case LINK_TABLE_END:
              break;
            case LINK_SYMBOL_OFFSET:
              lp = lp + 1;
              lp = lp + symlink_v3(lp, Ptr<u8>(ofh->code_infos[m_segment_process].offset));
              break;
            case LINK_TYPE_PTR:
              lp = lp + 1;  // seek past id
              lp = lp + typelink_v3(lp, Ptr<u8>(ofh->code_infos[m_segment_process].offset));
              break;
            case LINK_DISTANCE_TO_OTHER_SEG_64:
              lp = lp + 1;
              lp = lp + cross_seg_dist_link_v3(lp, ofh, m_segment_process, 8);
              break;
            case LINK_DISTANCE_TO_OTHER_SEG_32:
              lp = lp + 1;
              lp = lp + cross_seg_dist_link_v3(lp, ofh, m_segment_process, 4);
              break;
            case LINK_PTR:
              lp = lp + 1;
              lp = lp + ptr_link_v3(lp, ofh, m_segment_process);
              break;
            default:
              ASSERT_MSG(false, fmt::format("unknown link table thing {}", *lp));
              break;
          }
        }
      }

      m_segment_process++;
    } else {
      // all done, can set the entry point to the top-level.
      m_entry = Ptr<u8>(ofh->code_infos[TOP_LEVEL_SEGMENT].offset) + 4;
      return 1;
    }

    return 0;
  }

  else {
    printf("WORK v3 INVALID STATE\n");
    return 1;
  }
}

#define LINK_V2_STATE_INIT_COPY 0
#define LINK_V2_STATE_OFFSETS 1
#define LINK_V2_STATE_SYMBOL_TABLE 2
#define OBJ_V2_CLOSE_ENOUGH 0x90
#define OBJ_V2_MAX_TRANSFER 0x80000

uint32_t link_control::jak1_work_v2() {
  //  u32 startCycle = kernel.read_clock(); todo

  if (m_state == LINK_V2_STATE_INIT_COPY) {  // initialization and copying to heap
    // we move the data segment to eliminate gaps
    // very small gaps can be tolerated, as it is not worth the time penalty to move large objects
    // many bytes. if this requires copying a large amount of data, we will do it in smaller chunks,
    // allowing the copy to be spread over multiple game frames

    // state initialization
    if (m_segment_process == 0) {
      m_heap_gap =
          m_object_data - m_heap->current;  // distance between end of heap and start of object
    }

    if (m_heap_gap <
        OBJ_V2_CLOSE_ENOUGH) {  // close enough, don't relocate the object, just expand the heap
      if (link_debug_printfs) {
        printf("[work_v2] close enough, not moving\n");
      }
      m_heap->current = m_object_data + m_code_size;
      if (m_heap->top.offset <= m_heap->current.offset) {
        MsgErr("dkernel: heap overflow\n");  // game has ~% instead of \n :P
        return 1;
      }
    } else {  // not close enough, need to move the object

      // on the first run of this state...
      if (m_segment_process == 0) {
        m_original_object_location = m_object_data;
        // allocate on heap, will have no gap
        m_object_data = kmalloc(m_heap, m_code_size, 0, "data-segment");
        if (link_debug_printfs) {
          printf("[work_v2] moving from 0x%x to 0x%x\n", m_original_object_location.offset,
                 m_object_data.offset);
        }
        if (!m_object_data.offset) {
          MsgErr("dkernel: unable to malloc %d bytes for data-segment\n", m_code_size);
          return 1;
        }
      }

      // the actual copy
      Ptr<u8> source = m_original_object_location + m_segment_process;
      u32 size = m_code_size - m_segment_process;

      if (size > OBJ_V2_MAX_TRANSFER) {  // around .5 MB
        jak1::ultimate_memcpy((m_object_data + m_segment_process).c(), source.c(),
                              OBJ_V2_MAX_TRANSFER);
        m_segment_process += OBJ_V2_MAX_TRANSFER;
        return 0;  // return, don't want to take too long.
      }

      // if we have bytes to copy, but they are less than the max transfer, do it in one shot!
      if (size) {
        jak1::ultimate_memcpy((m_object_data + m_segment_process).c(), source.c(), size);
        if (m_segment_process > 0) {  // if we did a previous copy, we return now....
          m_state = LINK_V2_STATE_OFFSETS;
          m_segment_process = 0;
          return 0;
        }
      }
    }

    // otherwise go straight into the next state.
    m_state = LINK_V2_STATE_OFFSETS;
    m_segment_process = 0;
  }

  // init offset phase
  if (m_state == LINK_V2_STATE_OFFSETS && m_segment_process == 0) {
    m_reloc_ptr = m_link_block_ptr + 8;  // seek to link table
    if (*m_reloc_ptr == 0) {             // do we have pointer links to do?
      m_reloc_ptr.offset++;              // if not, seek past the \0, and go to next state
      m_state = LINK_V2_STATE_SYMBOL_TABLE;
      m_segment_process = 0;
    } else {
      m_base_ptr = m_object_data;  // base address for offsetting.
      m_loc_ptr = m_object_data;   // pointer which seeks thru the code
      m_table_toggle = 0;          // are we seeking or fixing?
      m_segment_process = 1;       // we've done first time setup
    }
  }

  if (m_state == LINK_V2_STATE_OFFSETS) {  // pointer fixup
    // this state reads through a table. Values alternate between "seek amount" and "number of
    // consecutive 4-byte
    //  words to fix up".  The counts are encoded using a variable length encoding scheme.  They use
    //  a very stupid
    // method of encoding values which requires O(n) bytes to store the value n.

    // to avoid dropping a frame, we check every 0x400 relocations to see if 0.5 milliseconds have
    // elapsed.
    u32 relocCounter = 0x400;
    while (true) {    // loop over entire table
      while (true) {  // loop over current mode

        // read and seek table
        u8 count = *m_reloc_ptr;
        m_reloc_ptr.offset++;

        if (!m_table_toggle) {  // seek mode
          m_loc_ptr.offset +=
              4 *
              count;  // perform seek (MIPS instructions are 4 bytes, so we >> 2 the seek amount)
        } else {      // offset mode
          for (u32 i = 0; i < count; i++) {
            if (m_loc_ptr.offset % 4) {
              ASSERT(false);
            }
            u32 code = *(m_loc_ptr.cast<u32>());
            code += m_base_ptr.offset;
            *(m_loc_ptr.cast<u32>()) = code;
            m_loc_ptr.offset += 4;
          }
        }

        if (count != 0xff) {
          break;
        }

        if (*m_reloc_ptr == 0) {
          m_reloc_ptr.offset++;
          m_table_toggle = m_table_toggle ^ 1;
        }
      }

      // reached the end of the tableToggle mode
      m_table_toggle = m_table_toggle ^ 1;
      if (*m_reloc_ptr == 0) {
        break;  // end of the state
      }
      relocCounter--;
      if (relocCounter == 0) {
        //        u32 clock_value = kernel.read_clock();
        //        if(clock_value - startCycle > 150000) { // 0.5 milliseconds
        //          return 0;
        //        }
        relocCounter = 0x400;
      }
    }
    m_reloc_ptr.offset++;
    m_state = 2;
    m_segment_process = 0;
  }

  if (m_state == 2) {  // GOAL object fixup
    if (*m_reloc_ptr == 0) {
      m_state = 3;
      m_segment_process = 0;
    } else {
      while (true) {
        u32 relocation = *m_reloc_ptr;
        m_reloc_ptr.offset++;
        Ptr<u8> goalObj;
        char* name;
        if ((relocation & 0x80) == 0) {
          // symbol!
          if (relocation > 9) {
            m_reloc_ptr.offset--;  // no idea what this is.
          }
          name = m_reloc_ptr.cast<char>().c();
          if (link_debug_printfs) {
            printf("[work_v2] symlink: %s\n", name);
          }
          goalObj = jak1::intern_from_c(name).cast<u8>();
        } else {
          // type!
          u8 nMethods = relocation & 0x7f;
          if (nMethods == 0) {
            nMethods = 1;
          }
          name = m_reloc_ptr.cast<char>().c();
          if (link_debug_printfs) {
            printf("[work_v2] symlink -type: %s\n", name);
          }
          goalObj = jak1::intern_type_from_c(name, nMethods).cast<u8>();
        }
        m_reloc_ptr.offset += strlen(name) + 1;
        // DECOMPILER->hookStartSymlinkV3(_state - 1, _objectData, std::string(name));
        m_reloc_ptr = c_symlink2(m_object_data, goalObj, m_reloc_ptr);
        // DECOMPILER->hookFinishSymlinkV3();
        if (*m_reloc_ptr == 0) {
          break;  // done
        }
        //        u32 currentCycle = kernel.read_clock();
        //        if(currentCycle - startCycle > 150000) {
        //          return 0;
        //        }
      }
      m_state = 3;
      m_segment_process = 0;
    }
  }
  m_entry = m_object_data + 4;
  return 1;
}

/*!
 * Complete linking. This will execute the top-level code for v3 object files, if requested.
 */
void link_control::jak1_finish(bool jump_from_c_to_goal) {
  ObjectFileHeader* ofh_pre = m_link_block_ptr.cast<ObjectFileHeader>().c();
  if (ofh_pre->object_file_version == 3) {
    // Version 3 moves each code segment into the GOAL heap, so m_code_start/m_code_size do not
    // describe the final executable ranges.  On Apple Silicon the JIT pages must be flipped
    // from writable to executable or the first call into linked GOAL code faults.
    for (u32 segment = 0; segment < ofh_pre->segment_count; ++segment) {
      const auto& code = ofh_pre->code_infos[segment];
      if (code.offset && code.size) {
        size_t executable_size = code.size;
#if defined(__APPLE__) && defined(__aarch64__)
        const u32 link_metadata = ofh_pre->link_infos[segment].size;
        if (link_metadata & LINK_ARM64_EXECUTABLE_SIZE_FLAG) {
          executable_size = link_metadata & ~LINK_ARM64_EXECUTABLE_SIZE_FLAG;
        }
#endif
        if (executable_size) {
#if OG_EXECUTION_MODE_AOT
          throw std::runtime_error("AOT object contains an executable code segment");
#else
          jit_memory::make_executable(Ptr<u8>(code.offset).c(), executable_size);
#endif
        }
      }
    }
  } else {
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
    // Legacy v2/v4 objects are relocated by GOAL after they are copied into
    // the heap. Keep their code range writable until that relocation method
    // has finished; x86 does not require this transition.
    if (m_code_size) {
      jit_memory::make_writable(m_code_start.c(), m_code_size);
    }
#else
    CacheFlush(m_code_start.c(), m_code_size);
#endif
  }
  auto old_debug_segment = DebugSegment;
  if (m_keep_debug) {
    // note - this probably doesn't work because DebugSegment isn't *debug-segment*.
    DebugSegment = s7.offset + jak1_symbols::FIX_SYM_TRUE;
  }
  if (m_flags & LINK_FLAG_FORCE_FAST_LINK) {
    FastLink = 1;
  }
  *EnableMethodSet = *EnableMethodSet + m_keep_debug;

  ObjectFileHeader* ofh = m_link_block_ptr.cast<ObjectFileHeader>().c();
  lg::debug("link finish: {}", m_object_name);
  partcensus_check(m_object_name);
  if (ofh->object_file_version == 3) {
    // todo check function type of entry

    // setup mips2c functions
    const auto& it = Mips2C::gMips2CLinkCallbacks[GameVersion::Jak1].find(m_object_name);
    if (it != Mips2C::gMips2CLinkCallbacks[GameVersion::Jak1].end()) {
      for (auto& x : it->second) {
        x();
      }
    }

    // execute top level!
    if (m_entry.offset && (m_flags & LINK_FLAG_EXECUTE)) {
      if (jump_from_c_to_goal) {
#if defined(__aarch64__)
        // AArch64 faults (EXC_ARM_SP_ALIGN) unless SP is 16-byte aligned.
        u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - 16;
#else
        u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - 8;
#endif
        call_goal_on_stack(m_entry.cast<Function>(), goal_stack, s7.offset, g_ee_main_mem);
      } else {
        call_goal(m_entry.cast<Function>(), 0, 0, 0, s7.offset, g_ee_main_mem);
      }
    }

    // inform compiler that we loaded.
    if (m_flags & LINK_FLAG_OUTPUT_LOAD) {
      output_segment_load(m_object_name, m_link_block_ptr, m_flags);
    }
  } else {
    if (m_flags & LINK_FLAG_EXECUTE) {
      auto entry = m_entry;
      auto name = basename_goal(m_object_name);
      strcpy(Ptr<char>(LINK_CONTROL_NAME_ADDR).c(), name);
      jak1::call_method_of_type_arg2(entry.offset, Ptr<jak1::Type>(*((entry - 4).cast<u32>())),
                                     GOAL_RELOC_METHOD, m_heap.offset,
                                     Ptr<char>(LINK_CONTROL_NAME_ADDR).offset);
    }
  }

  *EnableMethodSet = *EnableMethodSet - m_keep_debug;
  FastLink = 0;  // nested fast links won't work right.
  m_heap->top = m_heap_top;
  DebugSegment = old_debug_segment;
}

namespace jak1 {

/*!
 * Immediately link and execute an object file.
 * DONE, EXACT
 */
Ptr<uint8_t> link_and_exec(Ptr<uint8_t> data,
                           const char* name,
                           int32_t size,
                           Ptr<kheapinfo> heap,
                           uint32_t flags,
                           bool jump_from_c_to_goal) {
  link_control lc;
  lc.jak1_jak2_begin(data, name, size, heap, flags);
  uint32_t done;
  do {
    done = lc.jak1_work();
  } while (!done);
  lc.jak1_finish(jump_from_c_to_goal);
  return lc.m_entry;
}

/*!
 * Wrapper so this can be called from GOAL. Not in original game.
 */
u64 link_and_exec_wrapper(u64* args) {
  // data, name, size, heap, flags
  return link_and_exec(Ptr<u8>(args[0]), Ptr<char>(args[1]).c(), args[2], Ptr<kheapinfo>(args[3]),
                       args[4], false)
      .offset;
}

/*!
 * GOAL exported function for beginning a link with the saved_link_control
 * 47 -> output_load, output_true, execute, 8, force fast
 * 39 -> no 8 (s7)
 */
uint64_t link_begin(u64* args) {
  // object data, name size, heap flags
  saved_link_control.jak1_jak2_begin(Ptr<u8>(args[0]), Ptr<char>(args[1]).c(), args[2],
                                     Ptr<kheapinfo>(args[3]), args[4]);
  auto work_result = saved_link_control.jak1_work();
  // if we managed to finish in one shot, take care of calling finish
  if (work_result) {
    // called from goal
    saved_link_control.jak1_finish(false);
  }

  return work_result != 0;
}

/*!
 * GOAL exported function for doing a small amount of linking work on the saved_link_control
 */
uint64_t link_resume() {
  auto work_result = saved_link_control.jak1_work();
  if (work_result) {
    // called from goal
    saved_link_control.jak1_finish(false);
  }
  return work_result != 0;
}

/*!
 * The ULTIMATE MEMORY COPY
 * IT IS VERY FAST
 * but it may use the scratchpad.  It is implemented in GOAL, and falls back to normal C memcpy
 * if GOAL isn't loaded, or if the alignment isn't good enough.
 */
void ultimate_memcpy(void* dst, void* src, uint32_t size) {
  // only possible if alignment is good.
  if (!(u64(dst) & 0xf) && !(u64(src) & 0xf) && !(u64(size) & 0xf) && size > 0xfff) {
    if (!gfunc_774.offset) {
      // GOAL function is unknown, lets see if its loaded:
      auto sym = jak1::find_symbol_from_c("ultimate-memcpy");
      if (sym->value == 0) {
        memmove(dst, src, size);
        return;
      }
      gfunc_774.offset = sym->value;
    }

    Ptr<u8>(call_goal(gfunc_774, make_u8_ptr(dst).offset, make_u8_ptr(src).offset, size, s7.offset,
                      g_ee_main_mem))
        .c();
  } else {
    memmove(dst, src, size);
  }
}
}  // namespace jak1
