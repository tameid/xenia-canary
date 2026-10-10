/*
 * nui_ipc_logic.c - the pure logic of the frame IPC.
 *
 * Separated from the syscall layer so it can be tested on the host, where the
 * frame index and slot arithmetic live. Everything here is a function of its
 * arguments; nothing touches the guest's file system, and nothing depends on
 * 32-bit guest layouts.
 *
 * The two things worth testing are subtle and would both show up in the field
 * as "the guest never sees frames":
 *   - frame.idx is written by another process and may be read mid-write, so its
 *     parsing has to be strict;
 *   - the writer alternates slots, so "have I seen this frame" cannot be a
 *     simple comparison against the last slot.
 */

#include "nui_ipc.h"

void NuiIpcInit(NuiIpcState* state) {
  if (state == NULL) {
    return;
  }
  state->last_slot = 0xFFFFFFFFu;
  state->last_frame_id = 0xFFFFFFFFu;
  state->frames_seen = 0;
  state->decode_failures = 0;
  state->read_failures = 0;
  state->have_frame = 0;
}

int NuiIpcParseSlot(const unsigned char* text, unsigned int length,
                    unsigned int* slot_out) {
  if (text == NULL || slot_out == NULL) {
    return -1;
  }

  unsigned int slot = 0;
  unsigned int digits = 0;
  for (unsigned int i = 0; i < length; ++i) {
    const unsigned char c = text[i];
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
      continue;
    }
    if (c < '0' || c > '9') {
      return -1;
    }
    slot = slot * 10u + (unsigned int)(c - '0');
    ++digits;
    if (digits > 1u) {
      /* "0" or "1" only. Two digits means we are reading something else - a
       * torn index, or a file that is not the index at all. */
      return -1;
    }
  }
  if (digits == 0) {
    return -1;
  }
  *slot_out = slot;
  return 0;
}

int NuiIpcIsNew(const NuiIpcState* state, unsigned int slot) {
  if (state == NULL) {
    return 0;
  }
  if (state->last_slot == 0xFFFFFFFFu) {
    return 1; /* first frame of the session */
  }
  return slot != state->last_slot;
}

void NuiIpcMarkSeen(NuiIpcState* state, unsigned int slot, uint32_t frame_id) {
  if (state == NULL) {
    return;
  }
  state->last_slot = slot;
  state->last_frame_id = frame_id;
  ++state->frames_seen;
  state->have_frame = 1;
}