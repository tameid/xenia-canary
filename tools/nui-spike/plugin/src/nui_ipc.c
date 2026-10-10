/*
 * nui_ipc.c - guest side of the frame IPC: the syscall layer.
 *
 * STRUCT LAYOUTS BELOW ARE 32-BIT GUEST LAYOUTS. Every pointer is uint32_t and
 * the structs match xenia's definitions field for field:
 *   X_ANSI_STRING       {be u16 length, be u16 maximum_length, be u32 pointer}
 *   X_OBJECT_ATTRIBUTES {be u32 root_directory, be u32 name_ptr,
 *                        be u32 _reserved,      be u32 attributes}
 *
 * Do not "clean these up" to native pointers: the guest is a 32-bit PowerPC and
 * xenia reads these fields by offset. This is also why the layer is not
 * host-tested - the pure logic it shares with the host lives in
 * nui_ipc_logic.c.
 *
 *   signatures from src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:
 *   NtCreateFile takes 9 args; NtReadFile takes 8.
 *   ordinals from src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:
 *   NtCreateFile 0x0D2, NtReadFile 0x0F0, NtClose 0x0CF.
 *
 * The frame files are opened relative to the guest's current directory, which is
 * where the serving process launched us from. That is why the provider must be
 * pointed at that directory rather than an arbitrary path.
 */

#include "nui_ipc.h"

#define NUI_STATUS_SUCCESS 0x00000000u
#define NUI_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define NUI_STATUS_END_OF_FILE 0x00000103u

#define OBJ_INHERIT 0x00000002u
#define FILE_OPEN_IF 0x00000003u
#define FILE_NON_DIRECTORY_FILE 0x00000040u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u

/* Kept static rather than on the stack: the guest stack is small and these are
 * passed to the kernel by pointer, so they must stay put. */
static NuiAnsiString g_frame_index_name;
static NuiAnsiString g_frame_slot_name;
static NuiObjectAttributes g_object_attributes;
static unsigned int g_io_status_create[4];
static unsigned int g_io_status_read[4];
static char g_slot_chars[16];

int NuiIpcOpenRead(const char* name, unsigned int* handle_out) {
  /* Copy the name into guest-visible storage and build an ANSI_STRING over it.
   * `pointer` is a guest address, hence the uint32_t cast. */
  unsigned int length = 0;
  while (name[length] != 0) {
    ++length;
  }
  for (unsigned int i = 0; i < length; ++i) {
    g_slot_chars[i] = name[i];
  }
  g_slot_chars[length] = 0;

  g_frame_slot_name.length = (unsigned short)length;
  g_frame_slot_name.maximum_length = (unsigned short)(length + 1);
  g_frame_slot_name.pointer = (unsigned int)g_slot_chars;

  g_object_attributes.root_directory = 0;
  g_object_attributes.name_ptr = (unsigned int)&g_frame_slot_name;
  g_object_attributes.reserved = 0;
  g_object_attributes.attributes = OBJ_INHERIT;

  const unsigned int status = NtCreateFile(
      handle_out, GENERIC_READ, &g_object_attributes, g_io_status_create, 0, 0, 0,
      FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  return status == NUI_STATUS_SUCCESS ? 0 : -1;
}

int NuiIpcReadAll(unsigned int handle, void* buffer, unsigned int size,
                  unsigned int* bytes_out) {
  *bytes_out = 0;
  while (*bytes_out < size) {
    unsigned int got = 0;
    const unsigned int status = NtReadFile(
        handle, 0, 0, (unsigned char*)buffer + *bytes_out, size - *bytes_out,
        g_io_status_read, &got, 0);
    if (status == NUI_STATUS_END_OF_FILE) {
      break;
    }
    if (status != NUI_STATUS_SUCCESS) {
      return -1;
    }
    if (got == 0) {
      break;
    }
    *bytes_out += got;
  }
  return 0;
}

int NuiIpcPoll(NuiIpcState* state, NuiFrame* out, unsigned char* buffer,
               unsigned int buffer_size, unsigned char* index_scratch) {
  unsigned int handle = 0;

  if (NuiIpcOpenRead("frame.idx", &handle) != 0) {
    ++state->read_failures;
    return -1;
  }
  unsigned int read = 0;
  const int rc = NuiIpcReadAll(handle, index_scratch, 16, &read);
  NtClose(handle);
  if (rc != 0) {
    ++state->read_failures;
    return -1;
  }

  unsigned int slot = 0;
  if (NuiIpcParseSlot(index_scratch, read, &slot) != 0) {
    ++state->read_failures;
    return -1;
  }
  if (!NuiIpcIsNew(state, slot)) {
    return 0; /* nothing new */
  }

  /* Build "frame.<slot>.bin". */
  g_slot_chars[0] = 'f'; g_slot_chars[1] = 'r'; g_slot_chars[2] = 'a';
  g_slot_chars[3] = 'm'; g_slot_chars[4] = 'e'; g_slot_chars[5] = '.';
  g_slot_chars[6] = (char)('0' + slot);
  g_slot_chars[7] = '.'; g_slot_chars[8] = 'b'; g_slot_chars[9] = 'i';
  g_slot_chars[10] = 'n'; g_slot_chars[11] = 0;

  if (NuiIpcOpenRead(g_slot_chars, &handle) != 0) {
    /* The writer may be mid-publish; not an error worth reporting. */
    return -1;
  }
  const int frame_rc = NuiIpcReadAll(handle, buffer, buffer_size, &read);
  NtClose(handle);
  if (frame_rc != 0) {
    ++state->read_failures;
    return -1;
  }

  if (NuiDecodeFrame(buffer, read, out) != 0) {
    ++state->decode_failures;
    return -1;
  }

  NuiIpcMarkSeen(state, slot, out->header.frame_id);
  return 1;
}