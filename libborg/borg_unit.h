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

#include <stddef.h>
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

/* Ask the host to call this unit off the audio thread.
 *
 * unit_render is the audio thread: opening, reading or writing a device there
 * is a dropout waiting to happen. This is the opposite side of that rule. The
 * host calls `tick` on the panel thread, about twice a second at rest, and
 * after a MIDI read -- enough for a periodic device (a rawmidi node, a
 * helper's answer) without a thread of the unit's own. Twice a second is the
 * panel's cadence, not a frame rate: a unit driving something that is redrawn
 * faster (an Akai Fire at 50 Hz, say) runs a thread of its own and keeps this
 * as the fallback for a host where that thread cannot be created -- never both
 * at once, or the work is done twice. A tick is a pulse, not
 * a schedule: the host holds one call at a time and may skip one that arrives
 * while the previous is still running, so `tick` must keep each visit bounded
 * and must never block.
 *
 * `desc` is the same static key given to borg_unit_register, and unregistering
 * the page also removes the tick. Passing a NULL `tick` stops it. Returns 0 on
 * success and a negative value when the entry cannot be added. The unit must
 * check this symbol for NULL before calling it, like the other two.
 */
BORG_UNIT_WEAK int borg_unit_set_tick(const borg_unit_desc* desc,
                                      void (*tick)(void* user), void* user);

/* ------------------------------------------------------------------------- */
/* Transport services                                                         */
/* ------------------------------------------------------------------------- */

/* A second, additive interface, as a separate entry point rather than more
 * fields on borg_unit_desc: the desc is the page ABI and a unit built against
 * it has to keep loading, and a unit that never asks for this sees no change.
 *
 * What is reachable, and what is not, is worth stating once. A unit has no way
 * to reach the daemon's transport: drumlogued resolves twenty-three unit
 * symbols and none of them is a transport or a clock, and it does not answer
 * MIDI Start or Stop. borg has no better view of the daemon's own state --
 * except that it sits on the two places that transport touches the outside
 * world:
 *
 *   the panel     borg already rewrites every key frame the daemon reads. A
 *                 PLAY or STOP frame synthesized into that stream is a real
 *                 press as far as the daemon is concerned.
 *   the MIDI out  the daemon writes timing clock there while its sequencer
 *                 runs, and borg already counts every byte written. Clock
 *                 arriving, and clock stopping, is the one running/stopped
 *                 signal reachable from inside the process.
 *
 * Which side is master is the unit's decision, not borg's: borg reports what
 * it observes and presses a key when asked to, nothing more. A unit that
 * follows reads transport() and starts and stops itself; a unit that leads
 * calls transport_request() when its own transport changes and ignores
 * transport(). Both ignored is a policy too -- it is what a unit that never
 * asks for this does.
 *
 * ABI 2 appends MIDI. A unit cannot open a port: the daemon holds the USB
 * gadget and both DIN sockets, and borg already sits on every read. A unit
 * that subscribes is handed each complete SysEx message whose first bytes
 * match its prefix, on the daemon's MIDI thread, and answers on the output
 * paired with the port the message arrived on. The prefix is the unit's own
 * filter: 0x7D is shared by every borg user, so a unit that asks for 0x7D
 * alone would be offered electrik's and the modules' traffic too.
 *
 * The callback runs serialized with the modules' midi_in. It must only copy
 * into a ring of its own and return; the unit's tick consumes the ring and
 * does the work. Nothing that blocks, allocates or touches the unit's player
 * belongs on that thread.
 *
 * borg's text wire (wire.h, 0x3E) and the modules' traffic are unaffected. A
 * unit built against ABI 2 asks for 2 and falls back to get(1) when an older
 * borg is present; the fallback loses MIDI, not the page or the transport. */
/* ABI 3 appends the analog voice link, so a unit that wants only MIDI asks
 * for 2 and keeps working. */
#define BORG_UNIT_HOST_ABI 3u
#define BORG_UNIT_HOST_ABI_MIDI 2u
#define BORG_UNIT_HOST_ABI_VOICE 3u

/* ABI 2. Complete SysEx, F0 through F7 inclusive. `port` is the paired output
 * handle and is opaque to the unit. */
typedef void (*borg_unit_midi_fn)(void* port, const uint8_t* data, unsigned n,
                                  void* user);

typedef struct borg_unit_host {
  uint32_t size;                 /* sizeof(borg_unit_host): append-only */
  uint32_t abi;
  /* 1 running, 0 stopped, -1 not known. Derived from the timing clock the
   * daemon writes to its MIDI outputs: clock inside the last few hundred
   * milliseconds is running, and its absence is stopped. A configuration that
   * writes no clock leaves this at -1 for ever, and a caller must treat -1 as
   * "no opinion" -- never as "stopped", or the first tick would stop a song
   * the device never started. */
  int (*transport)(void);
  /* Press the daemon's PLAY (running != 0) or STOP key. Returns 0 when the
   * press was queued, a negative value when it cannot be: the borg menu is
   * open (every key is suppressed there), no panel has been seen, or the key
   * index has not been measured on this build. The down frame goes out on the
   * next panel report and the release on the one after -- about half a
   * second, which is the panel's own cadence. */
  int (*transport_request)(int running);
  /* The last panel key borg saw, as the daemon numbers it, with 1 for down and
   * 0 for up written through `down`. -1 when nothing has been pressed since
   * the object loaded. borg names only the four keys it needs itself, so this
   * is how the others -- PLAY and STOP among them -- are measured: open the
   * borg menu, press the key, read the index. */
  int (*last_key)(int* down);
  /* ABI 2. Ask for the complete SysEx messages that start with `prefix`. The
   * callback runs on the daemon's MIDI thread with the message and the paired
   * output handle, and must only copy. Returns a positive token, or a negative
   * value when there is no room. `prefix_len` is 1 to 8 bytes and must begin
   * with F0 to be useful; the match is on the bytes that follow it too. */
  int (*midi_subscribe)(const uint8_t* prefix, unsigned prefix_len,
                        borg_unit_midi_fn in, void* user);
  /* ABI 2. Stop offering messages. Returns after a callback already running
   * has finished, so the unit may free its ring when this returns. A token
   * that was never issued is ignored. */
  void (*midi_unsubscribe)(int token);
  /* ABI 2. One non-blocking write to the output paired with `port`. Returns
   * the bytes accepted (0 to n), or a negative errno such as -EAGAIN when the
   * port has no room. The caller resumes at that offset on its next tick; a
   * unit must never wait here, because a full USB buffer can outlast a tick. */
  int (*midi_try_write)(void* port, const void* data, size_t n);
  /* ABI 2. The name the daemon opened the port with, such as `hw:2,0,0`, or an
   * empty string. The pointer stays valid while the daemon runs. */
  const char* (*port_name)(void* port);
  /* ABI 3. The daemon's voice link, /dev/ttymxc1. The daemon owns the port,
   * so borg writes through its fd and taps the replies. `voice_write` queues
   * every byte and returns the count, or a negative errno; `voice_read` never
   * blocks and is byte oriented. */
  int (*voice_write)(const void* data, size_t n);
  int (*voice_read)(void* data, size_t n);
  /* ABI 3. Keep the daemon's preload from overwriting one transient slot. borg
   * drops its complete 0x81 frames for the slot and answers them itself. A
   * negative bank or slot releases. Process-wide, one slot at a time. */
  int (*voice_claim)(int bank, int slot);
} borg_unit_host;

/* NULL when borg is not in this process, or when it speaks a lower interface
 * than `abi`. The pointer is static and lives as long as the process, so one
 * lookup is enough -- and the lookup belongs after unit_init, never at load. */
BORG_UNIT_WEAK const borg_unit_host* borg_unit_host_get(uint32_t abi);

#ifdef __cplusplus
}
#endif

#endif
