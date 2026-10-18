# Borg


A drumlogue unit that switches `drumlogued` to run extensions in place.

Select the unit, fire `GateOn()`. The daemon re-executes itself under an
embedded preload object: same pid, same process, no rootfs writes. A power cycle
restores everything.


## Build

Requires podman and a logue-sdk checkout.

```sh
LOGUE_SDK_PATH=~/Projects/logue/logue-sdk ./build-drum.sh borg_drumlogue
```

The SDK container image defaults to `docker.io/xiashj/logue-sdk:latest` and can
be overridden:

```sh
CONTAINER_IMAGE=... ./build-drum.sh borg_drumlogue
```

Output: `bin/borg_drumlogue.drmlgunit`.


## Install and use

1. Copy `bin/borg_drumlogue.drmlgunit` to `userfs/Units/Synths/` on the
   instrument.
2. Load the unit on a part.
3. Save the program. The daemon reloads the saved program after the switch;
   without the unit in it, the confirmation screen will not return.
4. Fire `GateOn()`.


| STATE | Screen | Meaning |
|---|---|---|
| `!ready` | `BORG READY` / `PRESS A PAD` | waiting for `GateOn()` |
| `!switch` | `BORG SWITCHING` / `LOADING EXTENSIONS` | switch in progress |
| `!active` | `BORG ACTIVE` / `EXTENSIONS LOADED` | the object is loaded; gates are ignored |
| `!err` | `BORG FAILED` / `NOTHING CHANGED` | the switch was refused; the daemon is unchanged |


## Logs

| Path | Contents |
|---|---|
| `userfs/Programs/borg-exec.log` | pre-switch state and any refusal |
| `userfs/Programs/borg-shim.log` | attach confirmation from the loaded object |
| `/tmp/logue-borg-unit.log` | unit-side errors |


## Notes

- A failed switch leaves the daemon unchanged. The reason is in
  `borg-exec.log`.
- The switch closes every descriptor at 3 and above, so USB MIDI connections
  drop. Reconnect them afterwards if needed.
- Nothing is installed permanently. The staged object is in `/tmp`, which is
  gone at the next power cycle.

