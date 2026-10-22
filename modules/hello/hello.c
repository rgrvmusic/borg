/* The smallest useful .borgmodule: one tab and one field.
 *
 * It proves the loader, the ABI handshake and the menu page without touching
 * audio or MIDI. Build it with ../../build-module.sh and copy the result
 * directly to userfs/Programs/ on the instrument. */

#include "borg_module.h"

static const borg_host* g_host;

static int hello_init(const borg_host* host, const char* module_path) {
  (void)module_path;
  g_host = host;
  if (host->log != NULL) host->log("hello: ready");
  return 0;
}

static void hello_page(void) {
  if (g_host != NULL && g_host->row != NULL) {
    g_host->row(26, "MODULE:", "HELLO MODULE");
  }
}

static const borg_module hello_module = {
  BORG_MODULE_ABI,
  "HELLO",
  "1.0.0",
  BORG_CAP_PAGE,
  hello_init,
  hello_page,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,  /* audio_render (ABI 4) */
  NULL,  /* midi_msg (ABI 4) */
  NULL   /* midi_transform (ABI 5) */
};

const borg_module* borg_module_entry(uint32_t host_abi) {
  if (host_abi < 1) return 0;
  return &hello_module;
}
