# Where the image comes from

The receiver image is built by `image/build-image.sh` from this repository and from public
sources only. This file names each of them, at the exact version the image embeds. No source
archive is attached to the releases: every source below is available from its origin.

## The recipe

| Part | Pin |
|---|---|
| Build container | `archlinux@sha256:52dfbd5afb23a220d5ee92dafcb4c788d96c026ed04179890c23adef4c897fa0` (pulled from the tag `base-20261018.0.612668`), in `image/build-env` |
| Package snapshot | Arch Linux Archive, `2026/10/20`: `https://archive.archlinux.org/repos/2026/10/20/`, in `image/build-env` and `image/archiso-profile/pacman.conf` |
| Package signatures | checked (`SigLevel = Required DatabaseOptional`) |

Every package of the image comes from that one snapshot. The list of what the image holds,
`packages.txt` (`pacman -Q` on the built root), is written by every build and attached to every
release.

## The kernel: linux-rt 7.2.11.rt3.arch1-1

The image embeds the Arch Linux package `linux-rt 7.2.11.rt3.arch1-1` (`uname -r`:
`7.2.11-rt3-arch1-1-rt`), taken from the snapshot above:

```
https://archive.archlinux.org/repos/2026/10/20/extra/os/x86_64/linux-rt-7.2.11.rt3.arch1-1-x86_64.pkg.tar.zst
```

It is built by the Arch Linux packaging recipe of `linux-rt`:

- recipe: `https://gitlab.archlinux.org/archlinux/packaging/packages/linux-rt`, tag
  `7.2.11.rt3.arch1-1`, commit `c484769e8b3a913dbee6725a7c3da67a44b60b6a`;
- kernel configuration: `config.x86_64` of that commit,
  `https://gitlab.archlinux.org/archlinux/packaging/packages/linux-rt/-/raw/c484769e8b3a913dbee6725a7c3da67a44b60b6a/config.x86_64`.

Its `PKGBUILD`, read at that tag on 2026-10-26, takes three sources (URLs written here with the
`PKGBUILD` variables expanded; each one answered when checked that day):

| Source | URL | sha512 in the `PKGBUILD` |
|---|---|---|
| Linux 7.2.11, kernel.org | `https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.11.tar.xz` (signature: `linux-7.2.11.tar.sign`, same folder) | `6d11c92c490c0e7bc3485c86b59fc1c6bb79eed2604726c7b398797b8c48a8cbe49273c23857ab12929365868442c1ed6ba1b1c64c7d0404befd3b5f902eb69c` |
| real-time patch, 7.2.11-rt3 | `https://gitlab.archlinux.org/archlinux/packaging/upstream/linux-rt/-/releases/v7.2.11-rt3-arch1/downloads/linux-rt-v7.2.11-rt3.patch.zst` (signature: the same name ending in `.sig`) | `46c6f3ecc269dfa48b179162aed0ad86c499f7343888e5a7d13ca6ac566a18bdcabe317930cb8e9601d48f2c9e431daf7b118877db8c1ad615e940e7b2e6d715` |
| Arch Linux patch, 7.2.11-rt3-arch1 | `https://gitlab.archlinux.org/archlinux/packaging/upstream/linux-rt/-/releases/v7.2.11-rt3-arch1/downloads/linux-rt-v7.2.11-rt3-arch1.patch.zst` (signature: the same name ending in `.sig`) | `5690fc480ed08c9eeeb6bf7f1090c03181d3d18aed538c3f23d0116f0d46dfdc35cb5d4c67a277a9f335342e143eaa310704047237a0d95e80d43e066a72ab34` |
| configuration | `config.x86_64` of the recipe commit above | `759c7f37d121cc7dbc4e691be83aa2341ee5c65080388bf8149c93e362d2e9801194c690e29283822d8d68b85bad613266c6a9343e85bfa571e4cb6f72e21d06` |

The kernel is licensed under GPL-2.0 (`LICENSES/GPL-2.0.txt`). The 15 kHz output comes from the
`radeon` driver of that kernel and from the EDID file of `hardware/edid/`; the kernel is not
patched for it.

## switchres v2.2.2

The daemon links switchres statically, built inside the container from:

```
https://github.com/antonioginer/switchres.git
commit fa18d11c948adea2787036d8d20ad532f9fd6cc0 (tag v2.2.2)
```

The recipe checks out that commit and checks `git rev-parse HEAD` against it (`image/build-env`).
It builds `libswitchres.a` with `libdrm` only. switchres is licensed under GPL-2.0-or-later, as
every compiled source file of it says in its header; it ships no license file at its root, so
the text of the GPL version 2 is in `LICENSES/GPL-2.0.txt`.

## Firmware

The image carries four packages built from `linux-firmware`, from the same snapshot:
`linux-firmware-radeon`, `linux-firmware-realtek`, `linux-firmware-whence` (the `WHENCE` file,
which says where each firmware file comes from and under which terms) and `amd-ucode` (AMD
processor microcode). Each one keeps its license text in the image, under
`/usr/share/licenses/`. The Radeon microcode is distributed under `LICENSES/LICENSE.radeon`,
which asks that its notice be reproduced in the documentation: this repository and the stick
reproduce it.

## Everything else

Every other package of the image comes from the snapshot above, at the version `packages.txt`
names; its source is the Arch Linux packaging recipe of the same name and version, under
`https://gitlab.archlinux.org/archlinux/packaging/packages/`. The receiver's own code is this
repository.
