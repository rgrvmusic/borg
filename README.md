Borg
====


A synth unit that switches `drumlogued` to run extensions in place. Select
the unit and fire `GateOn()`: the daemon re-executes itself under an embedded
preload object, same pid, with no rootfs writes.

![](./docs/borg-screen.png)


The loaded object adds a status menu, opened with MUTE and MASTER, an optional
MIDI note and velocity transform, and a loader for `.borgmodule` pages kept in
the instrument's userfs.

Build with

```sh 
./build-drum.sh borg_drumlogue
```

which you have to copy to `/Units/Synths`.

A basic module system is added as `.borgmodule` in `/Programs`

![](./docs/borg-module-hello.png)


Install and use are in [borg_drumlogue/README.md](borg_drumlogue/README.md).


This repo is self-contained and abstracts away some of the other experiments I have done.


> [!NOTE]
> "drumlogue" and "microKORG2" are trademarks of KORG Inc. Borg is independent work: it is not affiliated with, endorsed by or supported by KORG. Any damage is your own responsiblity. No reverse engineering or disassembly was used. This relies on public files provided by the logue SDK.
