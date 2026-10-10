// Guest-side decode of the NUI wire format.
//
// Every field is byte-swapped on read: the provider writes little-endian and the
// guest is big-endian. The bounds checks are not decoration - frame.N.bin is
// being written by another process while we read it, so a short or stale read
// is a normal occurrence, not an exceptional one.

#include "nui_guest.h"

#include <string.h>

uint32_t NuiFrameDiskSize(uint32_t skeletons) {
  return NUI_DISK_HEADER_SIZE + skeletons * NUI_DISK_SKELETON_SIZE;
}

int NuiDecodeHeader(const uint8_t* data, uint32_t size, NuiFrameHeader* out) {
  if (data == NULL || out == NULL || size < NUI_DISK_HEADER_SIZE) {
    return -1;
  }

  out->magic = NuiReadU32(data + 0);
  out->version = NuiReadU16(data + 4);
  out->flags = NuiReadU16(data + 6);
  out->frame_id = NuiReadU32(data + 8);
  out->capture_timestamp_us = NuiReadU64(data + 12);
  out->host_timestamp_us = NuiReadU64(data + 20);
  out->num_skeletons = NuiReadU32(data + 28);
  out->num_depth_frames = NuiReadU32(data + 32);
  out->num_ir_frames = NuiReadU32(data + 36);
  out->num_color_frames = NuiReadU32(data + 40);

  if (out->magic != NUI_GUEST_MAGIC) {
    return -2;
  }
  if (out->version != NUI_GUEST_VERSION) {
    return -3;
  }
  /* A count larger than the buffer can hold means we are looking at a torn or
   * foreign file. Refuse rather than walk off the end. */
  if (out->num_skeletons > NUI_DISK_MAX_SKELETONS) {
    return -4;
  }
  if (size < NuiFrameDiskSize(out->num_skeletons)) {
    return -5;
  }
  return 0;
}

int NuiDecodeSkeleton(const uint8_t* frame_data, uint32_t frame_size,
                      uint32_t index, NuiSkeleton* out) {
  if (frame_data == NULL || out == NULL) {
    return -1;
  }

  NuiFrameHeader header;
  const int header_rc = NuiDecodeHeader(frame_data, frame_size, &header);
  if (header_rc != 0) {
    return header_rc;
  }
  if (index >= header.num_skeletons) {
    return -6;
  }

  const uint8_t* p = frame_data + NUI_DISK_HEADER_SIZE +
                     index * NUI_DISK_SKELETON_SIZE;

  out->tracking_id = NuiReadU32(p + 0);
  out->seated = p[4];
  out->up_status = p[5];
  out->flags = p[6];
  /* p+7 is reserved0, p+8..9 reserved1, p+10..13 reserved2: skipped. */

  out->position[0] = NuiReadF32(p + 14);
  out->position[1] = NuiReadF32(p + 18);
  out->position[2] = NuiReadF32(p + 22);
  /* p+26..41 orientation: the guest's NUI surface does not consume skeleton
   * orientation for anything we do yet, so it is not decoded. Deliberate: if a
   * future title needs it, add it here with a test rather than speculatively. */
  out->up_vector[0] = NuiReadF32(p + 42);
  out->up_vector[1] = NuiReadF32(p + 46);
  out->up_vector[2] = NuiReadF32(p + 50);
  out->up_confidence = NuiReadF32(p + 54);
  out->screen_position[0] = NuiReadF32(p + 58);
  out->screen_position[1] = NuiReadF32(p + 62);
  out->screen_rect[0] = NuiReadF32(p + 66);
  out->screen_rect[1] = NuiReadF32(p + 70);
  out->screen_rect[2] = NuiReadF32(p + 74);
  out->screen_rect[3] = NuiReadF32(p + 78);

  /* Joints start at offset 82 within the skeleton. */
  const uint8_t* j = p + 82;
  for (unsigned i = 0; i < (unsigned)NUI_GUEST_JOINT_COUNT; ++i) {
    const uint8_t* q = j + (size_t)i * NUI_DISK_JOINT_SIZE;
    out->joints[i].position[0] = NuiReadF32(q + 0);
    out->joints[i].position[1] = NuiReadF32(q + 4);
    out->joints[i].position[2] = NuiReadF32(q + 8);
    out->joints[i].orientation[0] = NuiReadF32(q + 12);
    out->joints[i].orientation[1] = NuiReadF32(q + 16);
    out->joints[i].orientation[2] = NuiReadF32(q + 20);
    out->joints[i].orientation[3] = NuiReadF32(q + 24);
    out->joints[i].pos_confidence = NuiReadF32(q + 28);
    out->joints[i].orient_confidence = NuiReadF32(q + 32);
  }

  return 0;
}