/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

// Defined in xboxkrnl_nui.cc (part of the Phase 0 spike patches).
DECLARE_bool(nui_fake_device);

namespace xe {
namespace kernel {
namespace xboxkrnl {
// Defined below, next to the reasoning for it. Forward-declared so
// XUsbcamGetState_entry can call it.
void BroadcastNuiHardwareStatusChanged();
void ArmNuiHardwareStatusBroadcast();
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XUsbcamCreate_entry(dword_t buffer,
                                   dword_t buffer_size,  // 0x4B000 640x480?
                                   lpdword_t handle_out) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamCreate, kNone, kStub);

enum class XUsbCamState : uint32_t {
  CamNotConnected = 0x00000000,
  CamInitilizied = 0x00000001,
  CamConnected = 0x00000002,
};

dword_result_t XUsbcamGetState_entry() {
  // PHASE 0 SPIKE: with nui_fake_device, report the camera as initialised and
  // connected. The Kinect dashboard's NUI init chain is
  //   XamNuiGetDeviceStatus -> XUsbcamGetState -> ...
  // and it gives up (black screen) the moment this says "not connected"; no
  // frame data backs the claim, it only lets the guest walk its init path.
  if (cvars::nui_fake_device) {
    ArmNuiHardwareStatusBroadcast();
    return static_cast<uint32_t>(XUsbCamState::CamInitilizied) |
           static_cast<uint32_t>(XUsbCamState::CamConnected);
  }
  return static_cast<uint32_t>(XUsbCamState::CamNotConnected);
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamGetState, kNone, kStub);

// PHASE 2 SPIKE: tell the guest the NUI hardware appeared.
//
// Measured against the Kinect dashboard: its entire NUI call sequence is
//
//   XamNuiHudSetEngagedTrackingID(0)
//   XamNuiGetDeviceStatus(ptr)
//   XUsbcamGetState()
//   XamNotifyCreateListenerInternal(0x23, 1, 8)      <- and then it waits
//
// It never polls again. It registers a listener and blocks until something says
// the hardware changed, and only then probes the camera. So returning "connected"
// from GetState is necessary but not sufficient: on its own the dashboard sits
// there forever, because nothing ever tells it to look.
//
// The mask works out: kXNotificationSystemNUIHardwareStatusChanged is 0x00060019,
// which XNotificationKey (xnotifylistener.h:25-41) splits into local_id 0x19,
// version 6, mask_index 0 - so it is delivered to any listener whose mask has bit
// 0 set. The dashboard's 0x23 does (0x23 & 1 == 1), and version 6 is within the
// max_version of 8 it registered.
//
// Broadcast once, on the first GetState that reports connected, because that is
// the transition a real device would produce. Broadcasting on every call would
// wake the guest in a loop and hide the real behaviour.
//
// Ordering matters and the obvious choice is wrong. Broadcasting from GetState
// was tried first and does not work: measured against the dashboard, GetState is
// called at log line ~2030 while the listener that cares is registered at ~2414,
// 380-odd lines later. The notification is enqueued into a listener that does
// not exist yet and is simply dropped - BroadcastNotification walks the current
// listener list and has no queue for listeners yet to come.
//
// So the broadcast is deferred: armed by GetState, fired later from the guest's
// own notification pump once there is something to deliver to. XeXamNotify-
// CreateListener is the one point guaranteed to run after the dashboard has
// registered, because it is the registration itself.
namespace {
bool g_nui_status_pending = false;
}

void ArmNuiHardwareStatusBroadcast() {
  g_nui_status_pending = cvars::nui_fake_device;
}

void BroadcastNuiHardwareStatusChanged() {
  if (!g_nui_status_pending) {
    return;
  }
  g_nui_status_pending = false;
  XELOGD("Broadcasting kXNotificationSystemNUIHardwareStatusChanged (0x00060019)");
  kernel_state()->BroadcastNotification(
      kXNotificationSystemNUIHardwareStatusChanged,
      static_cast<uint32_t>(XUsbCamState::CamInitilizied) |
          static_cast<uint32_t>(XUsbCamState::CamConnected));
}

// PHASE 2 SPIKE: the rest of the XUsbcam* family.
//
// Measured against the Kinect dashboard ($flash_dash.xex, NXE Kinect flash)
// from its own import table: it imports the whole family and uses them as a
// camera driver - Create, SetCaptureMode, SetConfig, GetState, ReadFrame. Only
// Create and GetState had any implementation at all; the other five were
// unresolved imports (kernel_module.cc returns 0 for them, so the guest gets a
// null import slot).
//
// These are logging stubs, not an implementation. Their purpose is to record
// what the guest actually asks for - capture mode, resolution, pixel format -
// which is what determines the frame layout, and which cannot be guessed from
// xenia's side. XUsbcamCreate already tells us the buffer is 0x4B000 = 307200 =
// exactly 640x480x1 byte, an 8-bit surface; whether that is IR or something
// else is decided by what SetCaptureMode/SetConfig request.
//
// Nothing here fabricates data. Returning success from ReadFrame with no
// pixels behind it would hand the guest a frame of whatever was in the buffer,
// which is worse than an explicit failure: it looks like a working camera and
// produces garbage downstream.

dword_result_t XUsbcamSetCaptureMode_entry(dword_t capture_mode) {
  XELOGD("XUsbcamSetCaptureMode({:08X})", static_cast<uint32_t>(capture_mode));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamSetCaptureMode, kNone, kStub);

dword_result_t XUsbcamSetConfig_entry(dword_t unknown1, dword_t config_ptr,
                                      dword_t unknown3) {
  // Log the pointed-to struct too: the capture settings live there, and the
  // pointer value alone says nothing about the requested geometry.
  XELOGD("XUsbcamSetConfig({:08X}, {:08X}, {:08X})",
         static_cast<uint32_t>(unknown1), static_cast<uint32_t>(config_ptr),
         static_cast<uint32_t>(unknown3));
  if (config_ptr) {
    const auto* words = kernel_memory()->TranslateVirtual<uint32_t*>(config_ptr);
    XELOGD("  config[0..7] = {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
           "{:08X}",
           words[0], words[1], words[2], words[3], words[4], words[5], words[6],
           words[7]);
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamSetConfig, kNone, kStub);

dword_result_t XUsbcamGetConfig_entry(lpvoid_t config_ptr) {
  XELOGD("XUsbcamGetConfig({:08X})", static_cast<uint32_t>(config_ptr));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamGetConfig, kNone, kStub);

dword_result_t XUsbcamSetView_entry(dword_t view) {
  XELOGD("XUsbcamSetView({:08X})", static_cast<uint32_t>(view));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamSetView, kNone, kStub);

dword_result_t XUsbcamReadFrame_entry(dword_t unknown1, dword_t unknown2,
                                      dword_t buffer_ptr,
                                      dword_t unknown4) {
  XELOGD("XUsbcamReadFrame({:08X}, {:08X}, {:08X}, {:08X})",
         static_cast<uint32_t>(unknown1), static_cast<uint32_t>(unknown2),
         static_cast<uint32_t>(buffer_ptr), static_cast<uint32_t>(unknown4));
  // No frame data exists behind this yet. Reporting success would return
  // whatever happens to be in the guest's buffer and look like a live camera,
  // so say so plainly instead.
  return X_E_NOT_IMPLEMENTED;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamReadFrame, kNone, kStub);

dword_result_t XUsbcamDestroy_entry(dword_t handle) {
  XELOGD("XUsbcamDestroy({:08X})", static_cast<uint32_t>(handle));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamDestroy, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Usbcam);
