/*
 * nui_frame.c - decode a whole frame buffer into skeletons.
 *
 * Split out from nui_decode.c so the frame-level logic can be host-tested
 * against frames produced by nui-provider, while nui_decode.c stays the raw
 * wire decoder. No guest-specific dependencies.
 */

#include "nui_frame.h"

int NuiDecodeFrame(const uint8_t* data, uint32_t size, NuiFrame* out) {
  if (out == NULL || out->skeletons == NULL) {
    return -1;
  }
  out->skeleton_count = 0;
  out->truncated = 0;

  NuiFrameHeader raw;
  const int rc = NuiDecodeHeader(data, size, &raw);
  if (rc != 0) {
    return rc;
  }

  /* Field-by-field rather than a struct assignment: a whole-struct copy lowers
   * to memcpy, and a freestanding guest has none. Extra lines, but it is the
   * difference between linking and not. */
  out->header.frame_id = raw.frame_id;
  out->header.flags = raw.flags;
  out->header.num_skeletons = raw.num_skeletons;
  out->header.capture_timestamp_us = raw.capture_timestamp_us;
  out->header.host_timestamp_us = raw.host_timestamp_us;
  out->header.num_depth_frames = raw.num_depth_frames;
  out->header.num_ir_frames = raw.num_ir_frames;
  out->header.num_color_frames = raw.num_color_frames;

  uint32_t count = raw.num_skeletons;
  if (count > out->capacity) {
    out->truncated = 1;
    count = out->capacity;
  }

  for (uint32_t i = 0; i < count; ++i) {
    if (NuiDecodeSkeleton(data, size, i, &out->skeletons[i]) != 0) {
      return -20 - (int)i;
    }
  }
  out->skeleton_count = count;
  return 0;
}