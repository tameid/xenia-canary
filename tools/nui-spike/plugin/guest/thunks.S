/*
 * thunks.S - XEX import thunk slots.
 *
 * Each 16-byte slot starts with the 32-bit XEX import record:
 *   bits 31..24 = record type (1 = function thunk)
 *   bits 15..0  = ordinal within the named library
 * xenia (cpu/xex_module.cc SetupLibraryImports) rewrites these 16 bytes at
 * load time into `sc 2 / blr / nop / nop` and hooks the syscall to the kernel
 * export, so the slot address doubles as the call target.
 *
 * ADDRESSES MUST MATCH mkxex.py. That file carries the authoritative table,
 * (library, ordinal, thunk address); this one carries the code. If they drift
 * the guest loads but calls land on the wrong exports, which is the kind of
 * failure that costs an afternoon.
 */

.macro THUNK name, ordinal
	.globl \name
	.type \name, @function
\name:
	.long (1 << 24) | (\ordinal)
	.long 0x60000000
	.long 0x60000000
	.long 0x60000000
	.size \name, 16
.endm

	.section .ximports, "ax", @progbits
	.align 4

/* xboxkrnl.exe */
	THUNK thunk_DbgPrint,                        0x003
	THUNK thunk_NtCreateFile,                    0x0D2
	THUNK thunk_NtReadFile,                      0x0F0
	THUNK thunk_NtClose,                         0x0CF

/* xam.xex - note xam.XEX, not xam.exe; xenia matches on the extension. */
	THUNK thunk_XamNuiGetDeviceStatus,           0x885
	THUNK thunk_XamNuiIsDeviceReady,             0x890
	THUNK thunk_XamNuiPlayerEngagementUpdate,    0x892
	THUNK thunk_XamNuiSkeletonGetBestSkeletonIndex, 0x893
	THUNK thunk_XNotifyBroadcast,                0x28E
	THUNK thunk_XMsgSystemProcessCall,           0x1F6

/* xboxkrnl.exe - guest-callable xconfig write, so the plugin can change NUI state. xconfig lives on the HOST; guest RAM cannot reach it. */
	THUNK thunk_ExReadModifyWriteXConfigSettingUlong, 0x2BC

/* xboxkrnl.exe - the camera driver. The plugin drives this sequence itself and
   prints every return, rather than inferring the contract from a title. */
	THUNK thunk_XUsbcamCreate,             0x222
	THUNK thunk_XUsbcamSetCaptureMode,     0x21A
	THUNK thunk_XUsbcamSetConfig,          0x21C
	THUNK thunk_XUsbcamGetConfig,          0x21B
	THUNK thunk_XUsbcamSetView,            0x220
	THUNK thunk_XUsbcamGetState,           0x21D
	THUNK thunk_XUsbcamReadFrame,          0x21E
	THUNK thunk_XUsbcamDestroy,            0x223

/* Alias the public names onto the slots so C calls ordinary symbols. */
	.set DbgPrint,                             thunk_DbgPrint
	.set NtCreateFile,                         thunk_NtCreateFile
	.set NtReadFile,                           thunk_NtReadFile
	.set NtClose,                              thunk_NtClose
	.set XamNuiGetDeviceStatus,                thunk_XamNuiGetDeviceStatus
	.set XamNuiIsDeviceReady,                  thunk_XamNuiIsDeviceReady
	.set XamNuiPlayerEngagementUpdate,         thunk_XamNuiPlayerEngagementUpdate
	.set XamNuiSkeletonGetBestSkeletonIndex,   thunk_XamNuiSkeletonGetBestSkeletonIndex
	.set XNotifyBroadcast,                     thunk_XNotifyBroadcast
	.set XMsgSystemProcessCall,                thunk_XMsgSystemProcessCall
	.set ExReadModifyWriteXConfigSettingUlong, thunk_ExReadModifyWriteXConfigSettingUlong

	.globl DbgPrint
	.globl NtCreateFile
	.globl NtReadFile
	.globl NtClose
	.globl XamNuiGetDeviceStatus
	.globl XamNuiIsDeviceReady
	.globl XamNuiPlayerEngagementUpdate
	.globl XamNuiSkeletonGetBestSkeletonIndex
	.globl XNotifyBroadcast
	.set XUsbcamCreate,           thunk_XUsbcamCreate
	.set XUsbcamSetCaptureMode,   thunk_XUsbcamSetCaptureMode
	.set XUsbcamSetConfig,        thunk_XUsbcamSetConfig
	.set XUsbcamGetConfig,        thunk_XUsbcamGetConfig
	.set XUsbcamSetView,          thunk_XUsbcamSetView
	.set XUsbcamGetState,         thunk_XUsbcamGetState
	.set XUsbcamReadFrame,        thunk_XUsbcamReadFrame
	.set XUsbcamDestroy,          thunk_XUsbcamDestroy
	.globl XMsgSystemProcessCall
	.globl ExReadModifyWriteXConfigSettingUlong
	.globl XUsbcamCreate
	.globl XUsbcamSetCaptureMode
	.globl XUsbcamSetConfig
	.globl XUsbcamGetConfig
	.globl XUsbcamSetView
	.globl XUsbcamGetState
	.globl XUsbcamReadFrame
	.globl XUsbcamDestroy