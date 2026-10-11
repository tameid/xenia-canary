/*
 * nui_plugin.c - the guest side of the Kinect pipeline.
 *
 * Polls the frame IPC nui-provider publishes, decodes the frames, and reports
 * what it got. Nothing is impersonated to xam yet: this stage is about proving
 * the guest can actually see the data, in a form readable from the log.
 *
 * Freestanding on purpose: no libc, no allocator, nothing but the thunks in
 * thunks.S. DbgPrint does NOT interpret printf specifiers, so every line is
 * assembled by hand - `SayLine` is the whole of that.
 *
 * Build: see the 360 build recipe in docs/kinect_spike_runbook.md 12.
 */

#include "nui_ipc.h"
#include "nui_frame.h"

/* xboxkrnl.exe ordinal 0x003 */
extern void DbgPrint(const char* message);

/* xam.xex ordinals - see thunks.S. */
extern u32 XamNuiGetDeviceStatus(void* status_ptr);
extern u32 XamNuiIsDeviceReady(void);
extern u32 XamNuiPlayerEngagementUpdate(u32 a, u32 b);
extern u32 XamNuiSkeletonGetBestSkeletonIndex(int index);

/* xboxkrnl.exe 0x2BC - category 3 (user), setting 0x0C (retail_flags). */
extern u32 ExReadModifyWriteXConfigSettingUlong(u16 category, u16 setting,
                                                u32 bit_affected, u32 flag);

/* How many poll iterations before giving up. A proof-of-concept run: enough
 * iterations that a live provider publishing at 30 Hz is certainly seen, and
 * bounded so a quiet run ends and prints its counters instead of spinning. */
#define NUI_PROBE_MAX_POLLS 400000u

/* --- hand-rolled formatting, because DbgPrint is not printf ------------- */

static void AppendText(char* at, unsigned int* at_index, const char* src) {
  while (*src != 0) {
    at[(*at_index)++] = *src++;
  }
}

static void AppendDecimal(char* at, unsigned int* at_index, unsigned int value) {
  char reversed[10];
  unsigned int n = 0;
  if (value == 0) {
    reversed[n++] = '0';
  }
  while (n < sizeof(reversed) && value > 0) {
    reversed[n++] = (char)('0' + (value % 10u));
    value /= 10u;
  }
  while (n > 0) {
    at[(*at_index)++] = reversed[--n];
  }
}

/* Prints "<label><value>\n" as a single DbgPrint call. */
static void SayLine(const char* label, unsigned int value) {
  static char line[96];
  unsigned int at = 0;
  AppendText(line, &at, label);
  AppendDecimal(line, &at, value);
  line[at++] = '\n';
  line[at] = 0;
  DbgPrint(line);
}

static void Say(const char* text) { DbgPrint(text); }

/* --- the probe ---------------------------------------------------------- */

static void probe(void) {
  /* XamNuiGetDeviceStatus writes 24 bytes; a real pointer or the guest hangs. */
  static u32 status_block[8];
  static NuiSkeleton skeletons[NUI_FRAME_MAX_SKELETONS];
  static unsigned char frame_buffer[NUI_IPC_MAX_FRAME_BYTES];
  static unsigned char index_scratch[16];

  NuiFrame frame;
  NuiIpcState ipc;

  frame.skeletons = skeletons;
  frame.capacity = NUI_FRAME_MAX_SKELETONS;

  Say("nui-plugin: start");
  NuiIpcInit(&ipc);

  /* Ambient device state first, so the log shows both halves. */
  SayLine("nui-plugin: is_device_ready=", XamNuiIsDeviceReady());
  SayLine("nui-plugin: get_device_status=", XamNuiGetDeviceStatus(status_block));

  /*
   * Set KinectInitialized before anything else touches device state.
   *
   * xconfig lives on the HOST, not in guest memory, so this cannot be done by
   * writing RAM - it has to go through an export. ExReadModifyWriteXConfig-
   * SettingUlong is the guest-callable one, and it is the same thing the
   * console-settings "Kinect Initialized" checkbox does.
   */
  SayLine("nui-plugin: set_flag=",
          ExReadModifyWriteXConfigSettingUlong(3u, 0x0Cu, 0x20000000u, 0x20000000u));

  /* Re-read to confirm the change was visible, rather than assuming it. */
  SayLine("nui-plugin: ready_after=", XamNuiIsDeviceReady());
  SayLine("nui-plugin: status_after=", XamNuiGetDeviceStatus(status_block));

  /*
   * The poll loop.
   *
   * No sleep: this is a proof-of-concept run, and a busy loop makes the
   * absence of frames obvious rather than hiding it. Failures are not reported
   * per iteration - the counters printed on exit say what happened.
   */
  for (u32 i = 0; i < NUI_PROBE_MAX_POLLS; ++i) {
    const int rc = NuiIpcPoll(&ipc, &frame, frame_buffer,
                              NUI_IPC_MAX_FRAME_BYTES, index_scratch);
    if (rc != 1) {
      continue;
    }

    const NuiSkeleton* s = &frame.skeletons[0];

    Say("nui-plugin: --- frame");
    SayLine("  frame_id=", frame.header.frame_id);
    SayLine("  skeletons=", frame.skeleton_count);
    SayLine("  head_y_x100=",
            (u32)(s->joints[NUI_JOINT_HEAD].position[1] * 100.0f));
    SayLine("  pelvis_y_x100=",
            (u32)(s->joints[NUI_JOINT_PELVIS].position[1] * 100.0f));
    SayLine("  tracking_id=", s->tracking_id);

    /*
     * Feed xam a player-engagement update derived from tracking quality.
     * Deliberately an obvious mapping: the point is to demonstrate the guest
     * can reach the export with values it derived itself and that xenia
     * accepts them. A real mapping comes from the title's own definitions.
     */
    XamNuiPlayerEngagementUpdate(
        s->tracking_id,
        (u32)(s->joints[NUI_JOINT_HEAD].pos_confidence * 100.0f));
  }

  Say("nui-plugin: --- summary");
  SayLine("  frames_seen=", ipc.frames_seen);
  SayLine("  read_failures=", ipc.read_failures);
  SayLine("  decode_failures=", ipc.decode_failures);
  SayLine("  best_skeleton=", XamNuiSkeletonGetBestSkeletonIndex(0));
  Say("nui-plugin: done");
}

void _start(void) {
  probe();
  for (;;) {
  }
}