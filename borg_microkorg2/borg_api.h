/**
 * @file borg_api.h
 * @brief The link between the unit and the resident shim -- both in one process.
 *
 * The unit and the preloaded object are two shared objects inside the same
 * microkorgd, but neither is linked against the other, so they share no
 * symbols. Everything they have to say to each other is a handful of events:
 * the chord completed, a menu key was pressed, and whether the menu is up.
 *
 * That used to be done with files under /tmp which the unit polled at 12 Hz.
 * It failed on the board in the one way a polled channel always can: the shim
 * saw the chord and withheld it (the key log records `drop 0f 1 shift=1` on
 * every press) while the unit never once observed the marker the shim had
 * written. Two independent clocks have to agree for a level-sampled event to
 * survive, and this one is as short as a button press.
 *
 * So the link is direct. The unit is dlopen'd by the daemon *after* this
 * object has been preloaded into its global scope, so the unit can dlsym()
 * the shim's register function at Init and hand it this struct: callbacks into
 * the unit, and the unit's own `menu open` flag for the shim to read. The
 * struct lives in the unit; the shim keeps only the pointer.
 *
 * The direction is unit -> shim by dlsym and shim -> unit by pointer on
 * purpose. The shim is loaded first and sits in the global scope, so the unit
 * can always find it; the unit is loaded with whatever visibility the daemon
 * picks, so RTLD_DEFAULT from the shim would not necessarily see it.
 *
 * Registration happens at the unit's Init, when the loader has long settled.
 * The note in mk2_shim.c about avoiding dlsym is about its own constructor,
 * which runs before the daemon's allocator exists -- not about this.
 */

#ifndef BORG_MICROKORG2_BORG_API_H
#define BORG_MICROKORG2_BORG_API_H

#define BORG_API_VERSION 2
/* FIVE, not four: the panel's label row has five fields and microkorgd leaves
 * the fifth empty on the OSC page. The unit puts CLOSE there, under the fifth
 * key, and the shim has to capture all five to give the row back on close. */
#define BORG_API_LABELS 5
#define BORG_API_LABEL_MAX 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct borg_unit_api {
  /* Checked by the shim; a mismatch means these two objects were built from
   * different sources, which can only happen if a stale shim was staged. */
  int version;

  /* The unit's flag: non-zero while the unit's menu is on the panel. Written
   * by the unit, read by the shim on every key frame -- the same thread, so no
   * locking is needed; `volatile` is for the compiler, not the CPU. */
  volatile int* menu_open;

  /* FUNCTION 3 + FUNCTION 5 completed (1) and released (0). Called from the
   * shim's read hook, i.e. on the daemon's panel thread. */
  void (*on_chord)(int down);

  /* A FUNCTION press that was withheld from the daemon because the menu is
   * open, in stream order. */
  void (*on_key)(unsigned id);

  /* The daemon's own soft-key labels, as the shim captured them on the way out
   * (MANUAL, not the BORG it rewrote). The shim fills these; the unit restores
   * them when the menu closes. */
  char (*labels)[BORG_API_LABEL_MAX];
  unsigned char* label_known;

  /* The unit's menu labels. Stable string literals; the shim rewrites the
   * daemon's own label frames to these while the menu is open, so a page
   * redraw cannot take the menu off the panel. */
  const char* menu_labels[BORG_API_LABELS];
} borg_unit_api;

/* Exported by the shim. Returns 1 when the shim took the struct. */
typedef int (*borg_register_fn)(borg_unit_api* api);
typedef void (*borg_unregister_fn)(borg_unit_api* api);

#ifdef __cplusplus
}
#endif

#endif
