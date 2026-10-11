/*
 * nui_ipc.h - guest side of the frame IPC.
 *
 * The syscall layer in nui_ipc.c is guest-only: its structs are 32-bit guest
 * layouts that xenia reads by offset, so it cannot be meaningfully tested on
 * the host. Everything below the "pure logic" marker is host-testable and is
 * tested in test_ipc_logic.c.
 */

#ifndef NUI_IPC_H_
#define NUI_IPC_H_

#include <stdint.h>

#include "nui_frame.h"
#include "nui_guest.h"

/* Guest-side integer type the plugin and its callers use. */
typedef uint32_t u32;

/* Frame buffer sizing: the header's wire maximum, not the decoded maximum, so
 * a file that claims more skeletons than we decode is refused rather than
 * silently truncated. */
#define NUI_IPC_MAX_SKELETONS 8

#define NUI_IPC_MAX_FRAME_BYTES \
  (NUI_DISK_HEADER_SIZE + NUI_IPC_MAX_SKELETONS * NUI_DISK_SKELETON_SIZE)

/* Desired-access for the frame files. */
#define GENERIC_READ 0x80000000u

/* Guest layouts, as xenia defines them. See nui_ipc.c for why these are not
 * native layout. */
typedef struct {
  uint16_t length;
  uint16_t maximum_length;
  uint32_t pointer;
} NuiAnsiString;

typedef struct {
  uint32_t root_directory;
  uint32_t name_ptr;
  uint32_t reserved;
  uint32_t attributes;
} NuiObjectAttributes;

/* IO_STATUS_BLOCK: xenia writes status at 0 and the byte count at 4. */
typedef struct {
  uint32_t status;
  uint32_t information;
} NuiIoStatusBlock;

typedef struct {
  uint32_t last_slot;
  uint32_t last_frame_id;
  uint32_t frames_seen;
  uint32_t decode_failures;
  uint32_t read_failures;
  int have_frame;
} NuiIpcState;

/* ---- pure logic, host-testable ---- */

void NuiIpcInit(NuiIpcState* state);

/* Parses the slot number out of frame.idx. Strict: whitespace is skipped, any
 * other non-digit is refused, and more than one digit is refused. Returns 0 and
 * sets *slot_out on success. */
int NuiIpcParseSlot(const unsigned char* text, unsigned int length,
                    unsigned int* slot_out);

/* Latest-frame-wins decision. 1 when `slot` is newer than the last consumed. */
int NuiIpcIsNew(const NuiIpcState* state, unsigned int slot);

void NuiIpcMarkSeen(NuiIpcState* state, unsigned int slot, uint32_t frame_id);

/* ---- syscall layer, guest only ---- */

int NuiIpcOpenRead(const char* name, unsigned int* handle_out);
int NuiIpcReadAll(unsigned int handle, void* buffer, unsigned int size,
                  unsigned int* bytes_out);
/* 1 = fresh frame in `out`, 0 = nothing new, -1 = read/decode failure. */
int NuiIpcPoll(NuiIpcState* state, NuiFrame* out, unsigned char* buffer,
               unsigned int buffer_size, unsigned char* index_scratch);

unsigned int NtCreateFile(unsigned int* handle_out, unsigned int desired_access,
                          void* object_attrs, void* io_status_block,
                          void* allocation_size_ptr, unsigned int file_attributes,
                          unsigned int share_access, unsigned int creation_disposition,
                          unsigned int create_options);
unsigned int NtReadFile(unsigned int file_handle, unsigned int event_handle,
                        void* apc_routine_ptr, void* apc_context,
                        NuiIoStatusBlock* io_status_block, void* buffer,
                        unsigned int buffer_length, void* byte_offset_ptr);
unsigned int NtClose(unsigned int handle);

#endif /* NUI_IPC_H_ */
