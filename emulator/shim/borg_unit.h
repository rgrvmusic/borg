/* Optional panel access for an ordinary drumlogue unit.
 *
 * A unit is a shared object loaded into drumlogued, so it can see symbols from
 * an object preloaded into that same process. This header declares the borg
 * side weak: when borg is not active the symbol resolves to zero and the unit
 * runs exactly as it did before. A unit that wants a panel page includes this
 * header and checks the symbol before using it.
 *
 * The page is drawn by borg on its panel thread, never from unit_render. The
 * unit publishes state from its audio callbacks and reads that state in
 * draw(). The callback is only called while its page is on the panel.
 *
 * The page area is the four rows below the tab bar. A row gives a 12-character
 * label column and a 12-character value column. A full-width line is at most
 * 25 characters. The panel stops drawing a text run at a comma, so use a space
 * instead. Fixed-size text only; the panel does not scroll.
 */

#ifndef BORG_UNIT_H
#define BORG_UNIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BORG_UNIT_ABI 1u
#define BORG_UNIT_TAB_MAX 8

#if defined(BORG_UNIT_BUILD)
/* The object that implements the API defines the symbols strongly. A unit
 * sees the declarations as weak, so an absent borg leaves them at zero. */
#define BORG_UNIT_WEAK
#elif defined(__GNUC__) || defined(__clang__)
#define BORG_UNIT_WEAK __attribute__((weak))
#else
#define BORG_UNIT_WEAK
#endif

typedef struct borg_unit_panel {
  uint32_t size;
  uint32_t abi;
  void (*text)(unsigned x, unsigned y, unsigned w, unsigned h, const char* s);
  void (*dark)(unsigned x, unsigned y, unsigned w, unsigned h, const char* s);
  void (*row)(unsigned y, const char* label, const char* value);
} borg_unit_panel;

typedef struct borg_unit_desc {
  uint32_t abi;
  /* The tab title, at most 7 characters. */
  const char* name;
  /* Called on the panel thread while this page is shown. Draw only inside the
   * call; the host services are not valid afterwards. */
  void (*draw)(const borg_unit_panel* panel, void* user);
  void* user;
} borg_unit_desc;

/* Add or replace this unit's page. The desc pointer is the key, so it must be
 * a static object that stays valid until unregister is called. Register from
 * unit_init or unit_resume; unregister from unit_teardown or unit_suspend.
 * Returns 0 on success and a negative value if the page cannot be added. The
 * unit must check the symbol for NULL before calling it. */
BORG_UNIT_WEAK int borg_unit_register(const borg_unit_desc* desc);

/* Remove the page. Safe to call when the page was never registered. */
BORG_UNIT_WEAK void borg_unit_unregister(const borg_unit_desc* desc);

#ifdef __cplusplus
}
#endif

#endif
