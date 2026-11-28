# nx-ovlloader (HOS 16.0.0+)

[![platform](https://img.shields.io/badge/platform-Switch-898c8c?logo=C.svg)](https://gbatemp.net/forums/nintendo-switch.283/?prefix_id=44)
[![language](https://img.shields.io/badge/language-C-ba1632?logo=C.svg)](https://github.com/topics/c)
[![ISC License](https://img.shields.io/badge/license-ISC-189c11.svg)](https://github.com/ppkantorski/nx-ovlloader/blob/main/LICENSE.md)
[![Latest Version](https://img.shields.io/github/v/release/ppkantorski/nx-ovlloader?label=latest&color=blue)](https://github.com/ppkantorski/nx-ovlloader/releases/latest)
[![GitHub Downloads](https://img.shields.io/github/downloads/ppkantorski/nx-ovlloader/total?color=6f42c1)](https://github.com/ppkantorski/nx-ovlloader/releases)
[![GitHub issues](https://img.shields.io/github/issues/ppkantorski/nx-ovlloader?color=222222)](https://github.com/ppkantorski/nx-ovlloader/issues)
[![GitHub stars](https://img.shields.io/github/stars/ppkantorski/nx-ovlloader)](https://github.com/ppkantorski/nx-ovlloader/stargazers)

**nx-ovlloader** is the loader sysmodule behind the [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) / Tesla overlay system on the Nintendo Switch. It starts at boot, maps overlay NROs into memory and chainloads `/switch/.overlays/ovlmenu.ovl`, so overlays can be opened from any game or application.

It is based on [nx-hbloader](https://github.com/switchbrew/nx-hbloader), with dynamic heap sizing, defaults that depend on the HOS version, an exit flag, and self-relaunch through [nx-ovlreloader](https://github.com/ppkantorski/nx-ovlreloader).

---

## Features

- **Overlay chainloading:** loads `/switch/.overlays/ovlmenu.ovl` (Ultrahand Overlay or Tesla Menu) at boot and again whenever an overlay exits.
- **Dynamic heap size:** read from `/config/nx-ovlloader/heap_size.bin`. Without it, the heap is 4 MB.
- **Heap changes without a reboot:** the heap size is checked before every overlay load, so a new size set in Ultrahand's System Settings applies right away.
- **Exit flag:** `/config/nx-ovlloader/exit_flag.bin` requests a clean exit instead of a reload. The flag is deleted as soon as it's read.
- **Auto-relaunch:** when the process exits, it uses `pmshell` to start [nx-ovlreloader](https://github.com/ppkantorski/nx-ovlreloader) (`0x420000000007E51B`), which starts nx-ovlloader again without restarting the console.
- **Rotating NRO addresses:** overlays are mapped within a fixed 64 GB to 256 GB window at 2 MB alignment. A random search is only used if that window runs out.
- **Load guard:** an atomic flag keeps two `loadNro()` calls from running at once.
- **Cached SD handle:** the SD card filesystem is opened once and reused, not reopened on every overlay switch.
- **Small footprint:** one FS session, a minimal directory cache and no CWD support, so it stays out of the way of everything else on the system.

---

## Installation

### Recommended (Ultrahand full package)

The easiest way is the complete [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) `sdout.zip`. Extract it to the root of your SD card. It includes nx-ovlloader, nx-ovlreloader, `ovlmenu.ovl` and the folders they need.

### Manual Install

1. Download the latest release zip from [Releases](https://github.com/ppkantorski/nx-ovlloader/releases/latest) and extract it to the root of your SD card.
    - It already has the right folder layout for both nx-ovlloader and nx-ovlreloader.
2. Put the overlay menu at `/switch/.overlays/ovlmenu.ovl`. This can be [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) or Tesla Menu.
3. Reboot your Switch. nx-ovlloader starts with Atmosphère and chainloads `ovlmenu.ovl`.

> **Note:** nx-ovlloader requires [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) and HOS 9.0.0 or later.

---

## File Layout

```
sdmc:/
├── atmosphere/
│   └── contents/
│       ├── 420000000007E51A/       ← nx-ovlloader sysmodule (Title ID)
│       │   ├── exefs.nsp           ← sysmodule binary
│       │   ├── toolbox.json        ← sysmodule info
│       │   └── flags/
│       │       └── boot2.flag      ← starts it at boot
│       └── 420000000007E51B/       ← nx-ovlreloader sysmodule (reloads on demand)
│           └── exefs.nsp
├── config/
│   └── nx-ovlloader/
│       ├── heap_size.bin           ← heap size (4 / 6 / 8 MB), written by Ultrahand
│       └── exit_flag.bin           ← exit request (deleted on the next load)
└── switch/
    └── Ultrahand-Reload/
        └── Ultrahand-Reload.nro    ← restarts nx-ovlloader from the hbmenu
```

---

## Building from Source

### Prerequisites

- [devkitPro](https://devkitpro.org) with `devkitA64` and `libnx`

### 1. Clone

```sh
git clone --recurse-submodules https://github.com/ppkantorski/nx-ovlloader.git
cd nx-ovlloader
```

### 2. Build

```sh
export DEVKITPRO=/opt/devkitpro
make
```

To build the full package with [nx-ovlreloader](https://github.com/ppkantorski/nx-ovlreloader), clone it into `external/nx-ovlreloader` and build it there too:

```sh
cd external/nx-ovlreloader
export DEVKITPRO=/opt/devkitpro
make
```

The Makefile uses all available CPU cores. The output is `ovll.nsp`, placed in `out/` with the Atmosphère folder layout.

### 3. Distribute (optional)

To make a zip that can be extracted straight to an SD card:

```sh
make dist
```

This creates `nx-ovlloader.zip` from the contents of `out/`.

---

## Contributing

Contributions are welcome. Open an [issue](https://github.com/ppkantorski/nx-ovlloader/issues/new) or send a [pull request](https://github.com/ppkantorski/nx-ovlloader/compare).

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/X8X3VR194) [![sponsor](https://github.com/ppkantorski/ppkantorski/raw/refs/heads/main/.pics/sponsor-icon.png)](https://github.com/sponsors/ppkantorski)

---

## License

Copyright © 2017-2018 nx-hbloader Authors

Copyright © 2020-2023 WerWolv

Copyright © 2024-2026 ppkantorski

Licensed under the [ISC License](LICENSE.md).
