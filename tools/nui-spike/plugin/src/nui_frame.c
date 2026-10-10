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
  out->header = raw;

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