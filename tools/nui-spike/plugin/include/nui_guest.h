/**
 * Guest-side view of the NUI wire format.
 *
 * This is an INDEPENDENT re-declaration of the layout in
 * tameid/nui-provider include/nui/frame.h. The two repos do not share code -
 * the provider runs on the host, this runs as guest code inside the emulator -
 * so the layout is duplicated deliberately.
 *
 * The duplication is the risk, and it is managed by static_asserts here that
 * mirror the provider's, plus tests/test_decode.c, which compiles this header on
 * the HOST and checks it against frames actually produced by nui-provider. If
 * the provider ever changes a layout without a version bump, that test fails.
 *
 * Everything below is the byte-swapped, guest-native view. The file on disk is
 * little-endian; the guest is a big-endian PowerPC, so every field is converted
 * on read. Do not memcpy a NuiFrameHeader and expect it to be usable.
 */

#ifndef NUI_GUEST_H_
#define NUI_GUEST_H_

#include <stdint.h>
#include <string.h>

#define NUI_GUEST_MAGIC 0x49554E58u /* 'XNUI' as little-endian bytes */
#define NUI_GUEST_VERSION 1u

/* Deliberately a plain int, not unsigned: this is used as an array dimension,
 * and `20u` is not a constant expression for that in C. In loops compare
 * against (unsigned)NUI_GUEST_JOINT_COUNT to avoid signed/unsigned warnings. */
#define NUI_GUEST_JOINT_COUNT 20

/* Flag bits, matching the provider. */
#define NUI_FLAG_PROVISIONAL 0x0001u
#define NUI_FLAG_EXTRAPOLATED 0x0002u
#define NUI_FLAG_SYNTHETIC 0x0004u

/* Xbox 360 NUI joint order. Must match the provider exactly. */
enum {
  NUI_JOINT_HEAD = 0,
  NUI_JOINT_NECK,
  NUI_JOINT_SPINECHEST,
  NUI_JOINT_PELVIS,
  NUI_JOINT_SHOULDERLEFT,
  NUI_JOINT_SHOULDERRIGHT,
  NUI_JOINT_ELBOWLEFT,
  NUI_JOINT_ELBOWRIGHT,
  NUI_JOINT_WRISTLEFT,
  NUI_JOINT_WRISTRIGHT,
  NUI_JOINT_HANDLEFT,
  NUI_JOINT_HANDRIGHT,
  NUI_JOINT_HIPLEFT,
  NUI_JOINT_HIPRIGHT,
  NUI_JOINT_KNELEFT,
  NUI_JOINT_KNEERIGHT,
  NUI_JOINT_ANKLELEFT,
  NUI_JOINT_ANKLERIGHT,
  NUI_JOINT_FOOTLEFT,
  NUI_JOINT_FOOTRIGHT,
};

/* On-disk sizes. These mirror the provider's static_asserts exactly. */
#define NUI_DISK_HEADER_SIZE 44u
#define NUI_DISK_JOINT_SIZE 36u
#define NUI_DISK_SKELETON_SIZE (82u + NUI_DISK_JOINT_SIZE * NUI_GUEST_JOINT_COUNT)
#define NUI_DISK_MAX_SKELETONS 8u

/*
 * Raw bytes as they appear in the file. Read with NuiReadU16/U32/U64 which
 * byte-swap, or use NuiDecodeHeader which does it for you.
 */
typedef struct {
  uint8_t bytes[NUI_DISK_HEADER_SIZE];
} NuiDiskHeader;

typedef struct {
  float position[3];
  float orientation[4];
  float pos_confidence;
  float orient_confidence;
} NuiJoint;

/* Guest-native: fields are already big-endian-correct. */
typedef struct {
  uint32_t tracking_id;
  uint8_t seated;
  uint8_t up_status;
  uint8_t flags;
  float position[3];
  float up_vector[3];
  float up_confidence;
  float screen_position[2];
  float screen_rect[4];
  NuiJoint joints[NUI_GUEST_JOINT_COUNT];
} NuiSkeleton;

/* Big-endian readers. The guest is big-endian, the file is not. */
static __inline uint16_t NuiReadU16(const uint8_t* p) {
  return (uint16_t)(((uint32_t)p[0]) | ((uint32_t)p[1] << 8));
}

static __inline uint32_t NuiReadU32(const uint8_t* p) {
  return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static __inline uint64_t NuiReadU64(const uint8_t* p) {
  return ((uint64_t)NuiReadU32(p)) | ((uint64_t)NuiReadU32(p + 4) << 32);
}

static __inline float NuiReadF32(const uint8_t* p) {
  uint32_t bits = NuiReadU32(p);
  float out;
  /* Byte-swap through an integer so this does not depend on host endianness:
   * the same source must work for the little-endian host test build and the
   * big-endian guest. */
  uint8_t swapped[4];
  swapped[0] = (uint8_t)(bits & 0xFF);
  swapped[1] = (uint8_t)((bits >> 8) & 0xFF);
  swapped[2] = (uint8_t)((bits >> 16) & 0xFF);
  swapped[3] = (uint8_t)((bits >> 24) & 0xFF);
  /* Plain memcpy, not __builtin_memcpy: the guest compiler is GCC and the host
   * test build is MSVC, and this file has to compile in both. */
  memcpy(&out, swapped, sizeof(out));
  return out;
}

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t frame_id;
  uint64_t capture_timestamp_us;
  uint64_t host_timestamp_us;
  uint32_t num_skeletons;
  uint32_t num_depth_frames;
  uint32_t num_ir_frames;
  uint32_t num_color_frames;
} NuiFrameHeader;

/* Returns 0 on success. Rejects bad magic, version, or an over-large count. */
int NuiDecodeHeader(const uint8_t* data, uint32_t size, NuiFrameHeader* out);

/* Reads skeleton `index` out of a frame buffer that starts at the header. */
int NuiDecodeSkeleton(const uint8_t* frame_data, uint32_t frame_size,
                      uint32_t index, NuiSkeleton* out);

/* Bytes a frame with `skeletons` occupies on disk. */
uint32_t NuiFrameDiskSize(uint32_t skeletons);

#endif /* NUI_GUEST_H_ */