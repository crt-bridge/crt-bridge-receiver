# crt-bridge receiver

The receiver turns a PC into a network display for a CRT. It boots from a USB stick, runs from
memory, and shows on the tube, at its native resolution and refresh rate, the picture an
emulator sends over the local network with the Groovy protocol (UDP port 32100; the gamepad,
keyboard and mouse of the receiver go back on port 32101). Nothing is installed on the PC's own
disks.

## The tube comes first

- **By default the receiver drives only its built-in 15 kHz band**, the one every
  standard-resolution CRT takes. It never leaves that band unless you describe your tube in a
  `monitor.ini` (see below), and a `monitor.ini` can only narrow the limits built into the
  receiver, never widen them.
- **The PC's BIOS shows its own screens at 31 kHz before any of this software runs** (the
  power-on screen, the boot menu). A 15 kHz-only tube receives that signal for a few seconds at
  every boot. This project cannot prevent it: judge whether your tube tolerates it before you
  connect it.
- The example file `examples/monitor-jvc_dtv1710cg.ini` on the stick describes one tube, a JVC
  DT-V1710CG. Besides the 15 kHz band it carries a 31 kHz band **verified on that tube only**.
  Do not use it for another tube.

## What has been verified

| Part | Verified | Not verified |
|---|---|---|
| Graphics card | one AMD Radeon HD 7750 (Cape Verde, PCI id `1002:683F`), `radeon` driver | any other card. Direct register writes are made on `1002:683F` only; any other card takes the slower, generic mode-setting path |
| Tube | one JVC DT-V1710CG | any other tube |
| PC | one PC, booted in BIOS mode | UEFI boot: provided, but **not verified on hardware** (tested in a virtual machine only) |
| Network card | the bench card, an Atheros L1 (`atl1` driver) | every other card, including the families listed below |

The image's kernel line names the bench card's connector (`DVI-I-1`) for the bench tube's
EDID; on another connector the receiver still starts and serves its built-in band.

## What the PC needs

- **4 logical CPUs minimum** (2 cores with Hyper-Threading, or 4 cores). This is not enforced:
  the receiver starts with fewer, and `report.txt` shows the number it found.
- A wired network connection. The address comes from DHCP; without a DHCP server the receiver
  keeps a `169.254.x.x` address. The address is shown on the tube.
- Booting from USB in BIOS mode. For UEFI, **Secure Boot must be disabled** in the firmware
  settings.

### Network cards

The image carries the firmware files of `linux-firmware-realtek`. Among the kernel's network
drivers, those that need a file from it are:

- wired Ethernet, PCI and PCIe: `r8169` (RTL8105E, RTL8106E, RTL8107E, RTL8125, RTL8126,
  RTL8127, RTL8168, RTL8402, RTL8411, RTL9151 families);
- wired Ethernet, USB adapters: `r8152` (RTL8153, RTL8156 families).

Drivers that need no firmware file at all, such as the Atheros `atl1`, `atl1c`, `atl1e` and
`atl2` drivers, do not depend on this package. None of these cards is verified except the bench
card. Wi-Fi and Bluetooth firmware come with the package, but the image configures neither.

## Writing the stick

Write the `.img` file of a release to a USB stick with Rufus, balenaEtcher or `dd`. The stick
holds one FAT32 partition named `CRTBRIDGE`, which Windows opens like any other stick.

**Writing the stick again erases the whole stick, `monitor.ini` and `settings.ini` included.**
Copy them elsewhere first.

## The files at the root of the stick

- `monitor.ini` (absent by default): describes your tube, in the `crt_range` format of
  switchres. Absent, the receiver uses its built-in 15 kHz band. A refused `monitor.ini` (a
  wrong value, or a file saved as "Unicode") stops the receiver instead of falling back, and
  `report.txt` says why. Files saved by Windows Notepad are accepted.
- `settings.ini`: volume, waiting screen, sound, inputs and extra options, each one explained in
  the file.
- `report.txt`: written by the receiver at every start. It tells the version, the kernel, the
  number of CPUs, the network cards and addresses, what was read from the two files above, and
  why the receiver runs or stopped. It holds the receiver's local address and the hardware
  address of its network card: look it over before you share it.
- `README.txt` says the same at more length, for the user of the stick.

## Developer mode

The image holds no ssh key and no password, and never starts sshd on its own. Adding two words
to the kernel command line turns on the developer mode for that boot:

```
crtbridge.dev=1 crtbridge.sshkey=<the base64 part of an ed25519 public key>
```

The boot line lives in `syslinux/syslinux.cfg` (BIOS) and `loader/entries/crtbridge.conf`
(UEFI) on the stick, or on your own network boot line. With it, sshd starts and lets `root` in
with that key only, and the receiver uses the example JVC file built into the image instead of a
`monitor.ini` on the stick. Without it, port 22 stays closed. An invalid key opens nothing, and
`report.txt` says so.

## Building the image from source

Needs a Linux x86_64 machine with bash, coreutils, sed, grep and a working Docker (one step runs
a `--privileged` container, as `mkarchiso` mounts inside its chroot), and access to
`archive.archlinux.org` and `github.com`. From the root of this repository:

```
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) bash image/build-image.sh . out
```

Every step runs in the Arch Linux container pinned by digest in `image/build-env`. Every package
comes from one dated Arch Linux Archive snapshot and is checked against its signature, and
switchres is built from its pinned commit. `out/` receives the stick image, the three network
boot files, `packages.txt` (every package in the image), `build-info.txt` (pins and checksums)
and `SHA256SUMS`. To boot the result in a virtual machine and read back its report:

```
bash image/qemu-smoke.sh bios out/crt-bridge-receiver-dev.img smoke
```

The daemon alone builds with `make -C src SWITCHRES_DIR=<a switchres tree where
make libswitchres has run>` (see `src/Makefile`).

## Not in this repository

An end-to-end guide, from an empty PC to a picture on the tube, is not part of this release.
The private test suite of the daemon is not published.

## Sources and licenses

- The receiver is licensed under GPL-3.0-or-later (`LICENSE`).
- switchres is linked statically into the daemon. It is licensed under GPL-2.0-or-later and
  ships no license file of its own; its text is in `LICENSES/GPL-2.0.txt`.
- The Radeon microcode in the image is distributed under the terms of
  `LICENSES/LICENSE.radeon`.
- Where the kernel, switchres and every other package of the image come from, with versions and
  commits: `SOURCES.md`.

Both license files of `LICENSES/` and `LICENSE` are also at the root of the stick.

## What this repository is

A public copy of the receiver as released; development happens elsewhere. This repository
receives one commit per synchronisation, never a history rewrite.

## Credits

Based on the Groovy protocol by psakhis: https://github.com/psakhis/Groovy_MiSTer

Mode lines by switchres, by Antonio Giner: https://github.com/antonioginer/switchres
