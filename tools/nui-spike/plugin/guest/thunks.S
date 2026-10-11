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

	.globl DbgPrint
	.globl NtCreateFile
	.globl NtReadFile
	.globl NtClose
	.globl XamNuiGetDeviceStatus
	.globl XamNuiIsDeviceReady
	.globl XamNuiPlayerEngagementUpdate
	.globl XamNuiSkeletonGetBestSkeletonIndex
	.globl XNotifyBroadcast
	.globl XMsgSystemProcessCall
