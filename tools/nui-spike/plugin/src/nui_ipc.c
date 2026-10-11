/*
 * nui_ipc.c - guest side of the frame IPC: the syscall layer.
 *
 * STRUCT LAYOUTS BELOW ARE 32-BIT GUEST LAYOUTS. Every pointer is uint32_t and
 * the structs match xenia's definitions field for field:
 *   X_ANSI_STRING       {be u16 length, be u16 maximum_length, be u32 pointer}
 *   X_OBJECT_ATTRIBUTES {be u32 root_directory, be u32 name_ptr,
 *                        be u32 _reserved,      be u32 attributes}
 *   X_IO_STATUS_BLOCK  {be u32 status, be u32 information}
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
 * THE RESULT IS IN THE STATUS BLOCK, NOT THE RETURN VALUE. xenia's NtReadFile
 * returns 259 (STATUS_END_OF_FILE) on a successful short read, while the io
 * status block carries SUCCESS and the byte count. Trust the block.
 *
 * The frame files live under \Device\Harddisk0\Partition1\, which is where
 * xenia maps the directory a standalone .xex was launched from. A bare
 * \frame.idx fails with "device not found".
 */
#include "nui_ipc.h"
#define NUI_STATUS_SUCCESS 0x00000000u
#define NUI_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define NUI_STATUS_END_OF_FILE 0x00000103u
#define OBJ_INHERIT 0x00000002u
#define FILE_OPEN_IF 0x00000003u
#define FILE_NON_DIRECTORY_FILE 0x00000040u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
/* Longest guest path we build, and longest bare file name. */
#define NUI_PATH_MAX 64u
#define NUI_NAME_MAX 24u
/* Kept static rather than on the stack: the guest stack is small and these are
 * passed to the kernel by pointer, so they must stay put. */
static NuiAnsiString g_frame_slot_name;
static NuiObjectAttributes g_object_attributes;
static char g_path[NUI_PATH_MAX];
static unsigned int g_io_status_create[4];
/*
 * Directory the frame files are read from, as a guest path.
 *
 * The guest's root has no device mounted, so a bare `\frame.idx` fails with
 * "device not found" - the same failure the dashboard hits on `\xam.xex`. For a
 * standalone .xex launched from a directory, xenia maps that directory to
 * \Device\Harddisk0\Partition1\. When the plugin is loaded from
 * storage_root()/plugins/<TITLE_ID>/ the right thing is the mounted `plugins:`
 * device instead; both resolve to the same directory on the host.
 */
#define NUI_FRAME_DIR "\\Device\\Harddisk0\\Partition1\\"

int NuiIpcOpenRead(const char* name, unsigned int* handle_out) {
  /* The frame directory is a guest path prefix; see NUI_FRAME_DIR. */
  const char* prefix = NUI_FRAME_DIR;
  unsigned int at = 0;
  while (prefix[at] != 0 && at < NUI_PATH_MAX - 2) {
    g_path[at] = prefix[at];
    ++at;
  }
  unsigned int i = 0;
  while (name[i] != 0 && at < NUI_PATH_MAX - 1) {
    g_path[at++] = name[i++];
  }
  g_path[at] = 0;
  g_frame_slot_name.length = (unsigned short)at;
  g_frame_slot_name.maximum_length = (unsigned short)NUI_PATH_MAX;
  g_frame_slot_name.pointer = (unsigned int)g_path;
  g_object_attributes.root_directory = 0;
  g_object_attributes.name_ptr = (unsigned int)&g_frame_slot_name;
  g_object_attributes.reserved = 0;
  g_object_attributes.attributes = OBJ_INHERIT;
  const unsigned int status = NtCreateFile(
      handle_out, GENERIC_READ, &g_object_attributes, g_io_status_create, 0, 0, 0,
      FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  return status == NUI_STATUS_SUCCESS ? 0 : -1;
}
int NuiIpcReadAllStatusBlock(unsigned int handle, void* buffer, unsigned int size,
                             unsigned int* bytes_out) {
  *bytes_out = 0;
  while (*bytes_out < size) {
    /* xenia reports the byte count in the status block's Information field,
     * not through a separate out-parameter. */
    static NuiIoStatusBlock iosb;
    iosb.status = 0;
    iosb.information = 0;

    NtReadFile(handle, 0, 0, 0, &iosb,
               (unsigned char*)buffer + *bytes_out,
               size - *bytes_out, 0);
    /* Trust the status block, not the return value: a successful read of
     * frame.idx returns 259 (STATUS_END_OF_FILE) while the status block says
     * SUCCESS with the byte count. See the note in NuiIpcPoll. */

    /* Zero bytes IS the end-of-stream signal, and it arrives before any
       failure: a stale or absent file surfaces as NO_SUCH_FILE with
       information == 0, so check that first or an EOF abort is a read
       failure and the frame is discarded. */
    if (iosb.information == 0) {
      break;
    }
    if (iosb.status != NUI_STATUS_SUCCESS) {
      return -1;
    }
    *bytes_out += iosb.information;
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
  /*
   * xenia puts the authoritative result in the io status block, NOT in the
   * return value. A successful read of a 2-byte frame.idx returns 259
   * (STATUS_END_OF_FILE) while the status block says SUCCESS with 2 bytes.
   * Trust the status block, and treat "information == 0" as the real
   * end-of-stream signal.
   */
  static NuiIoStatusBlock index_iosb;
  index_iosb.status = 0;
  index_iosb.information = 0;
  NtReadFile(handle, 0, 0, 0, &index_iosb, index_scratch, 16, 0);
  NtClose(handle);
  if (index_iosb.status != NUI_STATUS_SUCCESS) {
    ++state->read_failures;
    return -1;
  }
  const unsigned int index_bytes = index_iosb.information;
  if (index_bytes == 0) {
    return -1;
  }

  unsigned int slot = 0;
  if (NuiIpcParseSlot(index_scratch, index_bytes, &slot) != 0) {
    ++state->read_failures;
    return -1;
  }
  if (!NuiIpcIsNew(state, slot)) {
    return 0; /* nothing new */
  }
  /* Build "frame.<slot>.bin". */
  static const char kDigit[2] = {'0', '1'};
  char name[NUI_NAME_MAX];
  unsigned int n = 0;
  name[n++] = 'f'; name[n++] = 'r'; name[n++] = 'a'; name[n++] = 'm';
  name[n++] = 'e'; name[n++] = '.'; name[n++] = kDigit[slot];
  name[n++] = '.'; name[n++] = 'b'; name[n++] = 'i'; name[n++] = 'n';
  name[n] = 0;
  if (NuiIpcOpenRead(name, &handle) != 0) {
    /* The writer may be mid-publish; not an error worth reporting. */
    return -1;
  }
  unsigned int read = 0;
  const int frame_rc = NuiIpcReadAllStatusBlock(handle, buffer, buffer_size,
                                                &read);
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
