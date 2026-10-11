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

/* xboxkrnl.exe - the camera driver. Argument counts are not known; these are
 * the shapes xenia's own stub declares (xboxkrnl_usbcam.cc) and are being
 * confirmed here rather than assumed. */
extern u32 XUsbcamCreate(u32 buffer, u32 buffer_size, void* handle_out);
extern u32 XUsbcamSetCaptureMode(u32 capture_mode);
extern u32 XUsbcamSetConfig(u32 a, u32 config_ptr, u32 c);
extern u32 XUsbcamGetConfig(u32 config_ptr);
extern u32 XUsbcamSetView(u32 view);
extern u32 XUsbcamGetState(void);
extern u32 XUsbcamReadFrame(u32 a, u32 b, u32 buffer_ptr, u32 d);
extern u32 XUsbcamDestroy(u32 handle);

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

/* --- the camera driver, driven from here --------------------------------- */

/*
 * Why this exists, and why it is not the dashboard.
 *
 * The Kinect dashboard imports the whole XUsbcam* family but never gets far
 * enough to use most of it: it calls XamNuiGetDeviceStatus and XUsbcamGetState,
 * registers a notify listener for kXNotificationSystemNUIHardwareStatusChanged,
 * and then waits. It never calls Create, SetConfig, SetCaptureMode or
 * ReadFrame - so it cannot tell us what those want, and asking it costs a full
 * dashboard boot to learn nothing.
 *
 * Driving the sequence from the plugin means every call and every return is in
 * the log, in an order chosen here rather than one dictated by a title's init
 * path. What it cannot tell us is what a *real* title would pass - that still
 * needs a real title. What it does establish is the emulator side: whether the
 * exports are reachable, what they return, and what buffer geometry the device
 * accepts.
 */
static void ProbeCamera(void) {
  /* 0x4B000 is the size xenia's own XUsbcamCreate documents, and it is exactly
   * 640*480*1 - one 8-bit frame.
   *
   * Static, not stack: the guest stack is small and 307 KB would blow it. It has
   * to live in .bss, which sits after .data at 0x82010000 inside an image the
   * linker script lays out in 64 KiB pages - so it must be whole pages. Allocated
   * here rather than grown dynamically because this build is freestanding with
   * no allocator. */
  static unsigned char cam_buffer[0x4C000];  /* 0x4B000 rounded up to pages */
  static u32 config_block[16];
  u32 handle = 0;

  Say("nui-plugin: --- camera");

  config_block[0] = 0;
  SayLine("nui-plugin: cam_getstate=", XUsbcamGetState());

  SayLine("nui-plugin: cam_create=",
          XUsbcamCreate((u32)cam_buffer, 0x4B000u, &handle));
  SayLine("nui-plugin: cam_handle=", handle);

  /* Capture mode 0..3 as an enum is a guess; the point is the return, not the
   * meaning of the input. */
  u32 mode_rc = 0xFFFFFFFFu;
  for (u32 mode = 0; mode < 4; ++mode) {
    mode_rc = XUsbcamSetCaptureMode(mode);
    SayLine("nui-plugin: cam_setcapturemode_rc=", mode_rc);
  }

  SayLine("nui-plugin: cam_setconfig_rc=", XUsbcamSetConfig(0, (u32)config_block, 0));
  SayLine("nui-plugin: cam_getconfig_rc=", XUsbcamGetConfig((u32)config_block));
  SayLine("nui-plugin: cam_setview_rc=", XUsbcamSetView(0));
  SayLine("nui-plugin: cam_readframe_rc=", XUsbcamReadFrame(0, 0, (u32)cam_buffer, 0));
  SayLine("nui-plugin: cam_destroy=", XUsbcamDestroy(handle));

  /* Did anything land in the frame buffer? A device that reports success but
   * writes nothing is worse than one that fails, so check rather than trust. */
  unsigned int nonzero = 0;
  for (unsigned int i = 0; i < 0x4B000u; i += 0x100u) {
    if (cam_buffer[i] != 0) {
      ++nonzero;
    }
  }
  SayLine("nui-plugin: cam_buffer_nonzero_samples=", nonzero);

  Say("nui-plugin: --- camera end");
}

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

  ProbeCamera();

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