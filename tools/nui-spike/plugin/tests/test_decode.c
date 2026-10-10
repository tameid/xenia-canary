/*
 * Host test for the guest-side decoder.
 *
 * The decoder is the highest-risk part of the plugin: it is hand-written
 * byte-swapping over a layout duplicated from another repository, and getting
 * an offset wrong produces plausible-looking wrong poses rather than a crash.
 *
 * This compiles the SAME nui_decode.c that the guest plugin will use, on the
 * host, and checks it against frames actually produced by nui-provider. That
 * makes the endianness and the duplicated layout verifiable without a 360
 * toolchain - which is the point: the risky part gets tested now, and only the
 * xex packaging has to wait.
 *
 * Usage:
 *   nui_plugin_decode_test <frame.0.bin> <frame.1.bin> ...
 * or  nui_plugin_decode_test <a .nui session>   (first frame is extracted)
 *
 * Build: see tools/nui-spike/plugin/README.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nui_guest.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    ++g_checks;                                       \
    if (!(cond)) {                                    \
      ++g_failures;                                   \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
      printf(__VA_ARGS__);                            \
      printf("\n");                                   \
    }                                                 \
  } while (0)

static unsigned char* ReadFile(const char* path, long* out_size) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return NULL;
  }
  unsigned char* buf = (unsigned char*)malloc((size_t)size);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_size = size;
  return buf;
}

/* Extract the first frame out of a .nui session so this test can validate
 * recordings too, not just live IPC slots. */
static unsigned char* FirstFrameFromSession(const char* path, long* out_size) {
  long total = 0;
  unsigned char* raw = ReadFile(path, &total);
  if (!raw) {
    return NULL;
  }
  if (total < 60 || NuiReadU32(raw) != 0x53524E58u) {
    free(raw);
    return NULL;
  }
  const unsigned char* frame = raw + 60;
  const long remaining = total - 60;
  NuiFrameHeader header;
  if (NuiDecodeHeader(frame, (uint32_t)remaining, &header) != 0) {
    free(raw);
    return NULL;
  }
  const uint32_t size = NuiFrameDiskSize(header.num_skeletons);
  unsigned char* copy = (unsigned char*)malloc(size);
  if (!copy) {
    free(raw);
    return NULL;
  }
  memcpy(copy, frame, size);
  free(raw);
  *out_size = (long)size;
  return copy;
}

static void TestHeader(const unsigned char* data, long size) {
  printf("header\n");
  NuiFrameHeader header;
  const int rc = NuiDecodeHeader(data, (uint32_t)size, &header);
  CHECK(rc == 0, "decode rc=%d", rc);
  if (rc != 0) {
    return;
  }

  CHECK(header.magic == NUI_GUEST_MAGIC, "magic=%08X", header.magic);
  CHECK(header.version == NUI_GUEST_VERSION, "version=%u", header.version);
  CHECK(size >= (long)NuiFrameDiskSize(header.num_skeletons), "size too small");

  /* The synthetic backend stamps every frame; a real backend would not. Either
   * is fine, but the flag must be readable rather than garbage. */
  CHECK((header.flags & ~(NUI_FLAG_PROVISIONAL | NUI_FLAG_EXTRAPOLATED |
                          NUI_FLAG_SYNTHETIC)) == 0u,
        "unknown flag bits set: %04X", header.flags);

  /* Host timestamp must not precede capture: the provider writes them from the
   * same clock. */
  CHECK(header.host_timestamp_us >= header.capture_timestamp_us,
        "host %llu < capture %llu",
        (unsigned long long)header.host_timestamp_us,
        (unsigned long long)header.capture_timestamp_us);

  printf("  frame_id=%u skeletons=%u flags=0x%04X latency=%lluus\n",
         header.frame_id, header.num_skeletons, header.flags,
         (unsigned long long)(header.host_timestamp_us - header.capture_timestamp_us));
}

static void TestSkeleton(const unsigned char* data, long size) {
  printf("skeleton 0\n");
  NuiSkeleton skeleton;
  const int rc = NuiDecodeSkeleton(data, (uint32_t)size, 0, &skeleton);
  CHECK(rc == 0, "decode rc=%d", rc);
  if (rc != 0) {
    return;
  }

  CHECK(skeleton.tracking_id != 0, "tracking_id should be non-zero");

  /* The synthetic backend emits a standing figure with +Y up and the head
   * highest, so head.y must exceed pelvis.y. If the offsets were wrong by even
   * a field, this is what breaks. */
  const float head_y = skeleton.joints[NUI_JOINT_HEAD].position[1];
  const float pelvis_y = skeleton.joints[NUI_JOINT_PELVIS].position[1];
  const float foot_y = skeleton.joints[NUI_JOINT_FOOTLEFT].position[1];

  printf("  id=%u head.y=%.3f pelvis.y=%.3f foot.y=%.3f up=%.2f\n",
         skeleton.tracking_id, head_y, pelvis_y, foot_y, skeleton.up_confidence);

  CHECK(head_y > pelvis_y, "head %.3f should be above pelvis %.3f", head_y, pelvis_y);
  CHECK(pelvis_y > foot_y, "pelvis %.3f should be above foot %.3f", pelvis_y, foot_y);
  /* Up vector should be +Y and confident. */
  CHECK(skeleton.up_vector[1] > 0.9f, "up_vector.y=%.3f", skeleton.up_vector[1]);
  CHECK(skeleton.up_confidence > 0.9f, "up_confidence=%.3f", skeleton.up_confidence);

  /* Every joint must be a plausible number, not a garbage offset read. */
  int plausible = 0;
  for (uint32_t i = 0; i < NUI_GUEST_JOINT_COUNT; ++i) {
    const NuiJoint* j = &skeleton.joints[i];
    const float y = j->position[1];
    if (y > -1.0f && y < 3.0f) {
      ++plausible;
    }
  }
  CHECK(plausible == (int)NUI_GUEST_JOINT_COUNT, "%d/%u joints in a sane range",
        plausible, NUI_GUEST_JOINT_COUNT);

  /* Left and right shoulders must straddle the centre line. */
  const float sl = skeleton.joints[NUI_JOINT_SHOULDERLEFT].position[0];
  const float sr = skeleton.joints[NUI_JOINT_SHOULDERRIGHT].position[0];
  CHECK(sl < sr, "left shoulder %.3f should be left of right %.3f", sl, sr);
}

static void TestRejectsGarbage(void) {
  printf("rejects malformed input\n");
  unsigned char buf[NUI_DISK_HEADER_SIZE + NUI_DISK_SKELETON_SIZE];
  memset(buf, 0, sizeof(buf));

  NuiFrameHeader header;

  CHECK(NuiDecodeHeader(NULL, 100, &header) != 0, "null data accepted");
  CHECK(NuiDecodeHeader(buf, 0, &header) != 0, "empty accepted");
  CHECK(NuiDecodeHeader(buf, NUI_DISK_HEADER_SIZE - 1, &header) != 0,
        "short header accepted");

  /* All zeros: magic wrong. */
  CHECK(NuiDecodeHeader(buf, (uint32_t)sizeof(buf), &header) != 0,
        "zeroed header accepted");

  /* Valid magic/version but a skeleton count that cannot fit. */
  memset(buf, 0, sizeof(buf));
  buf[0] = 'X'; buf[1] = 'N'; buf[2] = 'U'; buf[3] = 'I';
  buf[4] = NUI_GUEST_VERSION; buf[5] = 0;
  buf[28] = 200; /* num_skeletons low byte */
  CHECK(NuiDecodeHeader(buf, (uint32_t)sizeof(buf), &header) != 0,
        "impossible skeleton count accepted");

  /* Index beyond the frame's count must be refused, not read past the end. */
  NuiFrameHeader one;
  memset(buf, 0, sizeof(buf));
  buf[0] = 'X'; buf[1] = 'N'; buf[2] = 'U'; buf[3] = 'I';
  buf[4] = NUI_GUEST_VERSION;
  buf[28] = 1; /* one skeleton */
  CHECK(NuiDecodeHeader(buf, (uint32_t)sizeof(buf), &one) == 0,
        "valid single-skeleton frame rejected");
  NuiSkeleton skeleton;
  CHECK(NuiDecodeSkeleton(buf, (uint32_t)sizeof(buf), 1, &skeleton) != 0,
        "out-of-range skeleton index accepted");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <frame.bin | session.nui> [...]\n", argv[0]);
    return 2;
  }

  for (int i = 1; i < argc; ++i) {
    printf("--- %s\n", argv[i]);
    long size = 0;
    const size_t len = strlen(argv[i]);
    unsigned char* data;
    if (len > 5 && strcmp(argv[i] + len - 4, ".nui") == 0) {
      data = FirstFrameFromSession(argv[i], &size);
    } else {
      data = ReadFile(argv[i], &size);
    }
    if (!data) {
      printf("  could not read %s\n", argv[i]);
      ++g_failures;
      continue;
    }

    TestHeader(data, size);
    TestSkeleton(data, size);
    free(data);
  }

  TestRejectsGarbage();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
