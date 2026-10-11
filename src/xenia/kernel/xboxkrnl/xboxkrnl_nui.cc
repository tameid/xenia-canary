/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// NUI device requests. These three ordinals exist in xboxkrnl_table.inc
// (0x30D/0x30E/0x30F) but have never had an implementation, so
// KernelModule::GetProcAddressByOrdinal returns 0 for them
// (kernel_module.cc:119-134) and the guest gets a null import.
//
// NUIView (the Xbox 360 Kinect developer test app) imports all three and calls
// PsCamDeviceRequest during NUI init, immediately after which it calls
// KeBugCheckEx. Making them return "no such device" is what XamNuiGetDeviceStatus
// and friends already expect: xam_nui.cc documents that a negative return is the
// "device absent" signal for all three (see the comments at xam_nui.cc:28-33 and
// :150-156, where DetroitDeviceRequest's result is only used to decide whether the
// tilt controller exists).

#include "xenia/base/logging.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

DECLARE_bool(nui_fake_device);

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Status used when no NUI device is present. Any negative value works for the
// callers in xam_nui.cc; this matches how the other device stubs report absence.
static constexpr uint32_t kDeviceNotPresent = 0xC0000001;  // STATUS_UNSUCCESSFUL

dword_result_t PsCamDeviceRequest_entry(dword_t request_code,
                                        dword_t request_buffer_ptr,
                                        dword_t buffer_size,
                                        dword_t result_buffer_ptr) {
  // "PS" camera = the Kinect's colour/IR/depth camera stack.
  XELOGD("PsCamDeviceRequest({:08X}, {:08X}, {:08X})",
         static_cast<uint32_t>(request_code),
         static_cast<uint32_t>(request_buffer_ptr),
         static_cast<uint32_t>(buffer_size));
  // With --nui_fake_device the device claims to be present. The data behind it
  // is not real; this exists so titles can be walked through their NUI
  // initialisation path and the emulator-side call surface recorded.
  return cvars::nui_fake_device ? 0 : kDeviceNotPresent;
}
DECLARE_XBOXKRNL_EXPORT_(PsCamDeviceRequest, kNone, xe::cpu::ExportTag::kStub);

dword_result_t McaDeviceRequest_entry(dword_t request_code,
                                      dword_t request_buffer_ptr,
                                      dword_t buffer_size,
                                      dword_t result_buffer_ptr) {
  // "Mca" = the Kinect's microphone array.
  XELOGD("McaDeviceRequest({:08X}, {:08X}, {:08X})",
         static_cast<uint32_t>(request_code),
         static_cast<uint32_t>(request_buffer_ptr),
         static_cast<uint32_t>(buffer_size));
  return cvars::nui_fake_device ? 0 : kDeviceNotPresent;
}
DECLARE_XBOXKRNL_EXPORT_(McaDeviceRequest, kNone, xe::cpu::ExportTag::kStub);

dword_result_t DetroitDeviceRequest_entry(dword_t request_code,
                                          dword_t request_buffer_ptr,
                                          dword_t buffer_size,
                                          dword_t result_buffer_ptr) {
  // "Detroit" = the tilt motor board. XamNuiCameraGetTiltControllerType counts
  // leading zeros in this return value, so 0 means "no tilt controller" and 1
  // means "tilt controller present" (see xam_nui.cc:150-158).
  XELOGD("DetroitDeviceRequest({:08X}, {:08X}, {:08X})",
         static_cast<uint32_t>(request_code),
         static_cast<uint32_t>(request_buffer_ptr),
         static_cast<uint32_t>(buffer_size));
  return cvars::nui_fake_device ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT_(DetroitDeviceRequest, kNone, xe::cpu::ExportTag::kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DEFINE_bool(
    nui_fake_device, false,
    "PHASE 0 SPIKE. Report the NUI device (PsCam/Mca/Detroit) as present even "
    "though no data backs it, so titles walk their NUI init path.\n"
    " Only for tracing which APIs a title calls; not a working Kinect.",
    "Kernel");

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Nui);