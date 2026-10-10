/* Host test for the pure IPC logic.
 *
 * test_ipc_logic.c runs on the host because NuiIpcParseSlot and NuiIpcIsNew
 * have no guest dependencies. It will fail if the provider ever writes a frame
 * index this logic cannot read, which is worth catching before the guest is
 * involved: the symptom in the guest is silence, not an error.
 */

#include <stdio.h>
#include <string.h>

#include "nui_ipc.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                            \
  do {                                              \
    ++g_checks;                                     \
    if (!(cond)) {                                  \
      ++g_failures;                                 \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
    }                                               \
  } while (0)

/* NuiIpcParseSlot takes bytes, not text: it reads a file that another process
 * is writing, so the caller has to pass a length. Cast the literals explicitly
 * rather than loosening the signature to char*. */
#define T(s) ((const unsigned char*)(s))

static void TestParseSlot(void) {
  printf("parse frame.idx\n");
  unsigned int slot;

  /* The normal case, with the newline the provider writes. */
  CHECK(NuiIpcParseSlot(T("0\n"), 2, &slot) == 0 && slot == 0, "plain 0");
  CHECK(NuiIpcParseSlot(T("1\n"), 2, &slot) == 0 && slot == 1, "plain 1");

  /* Whitespace the provider may write, and CRLF from a Windows text mode. */
  CHECK(NuiIpcParseSlot(T("1\r\n"), 3, &slot) == 0 && slot == 1, "CRLF");
  CHECK(NuiIpcParseSlot(T(" 1 "), 3, &slot) == 0 && slot == 1, "padded");

  /* Refusals. */
  CHECK(NuiIpcParseSlot(T(""), 0, &slot) != 0, "empty accepted");
  CHECK(NuiIpcParseSlot(T("\n"), 1, &slot) != 0, "newline-only accepted");
  CHECK(NuiIpcParseSlot(T("x"), 1, &slot) != 0, "letter accepted");
  CHECK(NuiIpcParseSlot(T("10\n"), 3, &slot) != 0, "two digits accepted");
  CHECK(NuiIpcParseSlot(NULL, 4, &slot) != 0, "null accepted");
  CHECK(NuiIpcParseSlot(T("1\n"), 2, NULL) != 0, "null out accepted");

  /* The dangerous one: a torn read. write-temp-then-rename means a torn index
   * is a *shorter* buffer, so the number may arrive without its newline. That
   * must still parse. */
  CHECK(NuiIpcParseSlot(T("0"), 1, &slot) == 0 && slot == 0,
        "missing newline should be tolerated");

  /* But a buffer with garbage after the number is not a torn read. */
  const unsigned char partial[] = {'1', '\n', 0x00, 0x00};
  CHECK(NuiIpcParseSlot(partial, sizeof(partial), &slot) != 0,
        "trailing NULs should not be tolerated as data");
}

static void TestIsNew(void) {
  printf("latest-frame-wins\n");
  NuiIpcState state;
  NuiIpcInit(&state);

  /* First poll: unknown slot, so anything is new. */
  CHECK(NuiIpcIsNew(&state, 0) == 1, "first frame not new");
  CHECK(NuiIpcIsNew(&state, 1) == 1, "first frame alt slot not new");

  NuiIpcMarkSeen(&state, 0, 100);
  CHECK(state.frames_seen == 1, "frames_seen not incremented");
  CHECK(state.have_frame == 1, "have_frame not set");

  /* Same slot again: the index was rewritten for the same frame. */
  CHECK(NuiIpcIsNew(&state, 0) == 0, "same slot should not be new");

  /* Writer alternates, so the other slot is new. */
  CHECK(NuiIpcIsNew(&state, 1) == 1, "alternating slot should be new");
  NuiIpcMarkSeen(&state, 1, 101);
  CHECK(NuiIpcIsNew(&state, 0) == 1, "back to first slot should be new");

  /* A fresh state must treat everything as new again. */
  NuiIpcState again;
  NuiIpcInit(&again);
  CHECK(NuiIpcIsNew(&again, 1) == 1, "reinitialised state");
  CHECK(again.frames_seen == 0, "reinitialised counters");

  /* Null must not crash. */
  CHECK(NuiIpcIsNew(NULL, 0) == 0, "null state");
  CHECK(NuiIpcIsNew(0, 0) == 0, "null state (literal)");
}

int main(void) {
  TestParseSlot();
  TestIsNew();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}