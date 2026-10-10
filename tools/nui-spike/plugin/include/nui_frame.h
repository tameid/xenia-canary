/*
 * nui_frame.h - a decoded frame.
 *
 * A view over a caller-owned buffer, so decoding never allocates: the guest has
 * no allocator worth relying on, and a frame is 1648 bytes.
 */

#ifndef NUI_FRAME_H_
#define NUI_FRAME_H_

#include <stdint.h>

#include "nui_guest.h"

/* Most skeletons a frame is decoded into. The provider's wire header allows 8;
 * a 5-player Kinect scene is not a case worth carrying weight for. Frames that
 * claim more are decoded up to this and flagged `truncated`. */
#define NUI_FRAME_MAX_SKELETONS 4

typedef struct {
  /* Caller supplies storage. */
  NuiSkeleton* skeletons;
  uint32_t capacity;

  /* Filled in by NuiDecodeFrame. Stored by value: it is 52 bytes, and the
   * caller would otherwise need to keep a separate local alive. */
  NuiFrameHeader header;
  uint32_t skeleton_count;
  int truncated;
} NuiFrame;

int NuiDecodeFrame(const uint8_t* data, uint32_t size, NuiFrame* out);

#endif /* NUI_FRAME_H_ */