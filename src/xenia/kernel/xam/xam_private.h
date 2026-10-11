/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_PRIVATE_H_
#define XENIA_KERNEL_XAM_XAM_PRIVATE_H_

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/xam/xam_ordinals.h"

namespace xe {
namespace kernel {
namespace xam {

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                  \
  void Register##n##Exports(xe::cpu::ExportResolver* export_resolver, \
                            KernelState* kernel_state);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam

// Phase 2 NUI spike. Fires an armed kXNotificationSystemNUIHardwareStatusChanged
// once a notify listener exists; armed by XUsbcamGetState. Declared here because
// xam_notify.cc needs it and it lives in xboxkrnl - but outside namespace xam,
// since placing it inside would capture the unqualified lookups every other xam
// source file makes into xe::kernel::xboxkrnl.
namespace xboxkrnl {
void ArmNuiHardwareStatusBroadcast();
void BroadcastNuiHardwareStatusChanged();
}  // namespace xboxkrnl

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_PRIVATE_H_
