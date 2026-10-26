#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make-usb-img.sh NETBOOT_DIR USB_DIR OUT_IMG [STICK_DIR]
#
# Wraps the three netboot artefacts of the receiver image into a raw disk
# image for a USB stick: an MBR partition table, one FAT32 partition (type
# 0x0C, active, starting at sector 2048, label CRTBRIDGE), the syslinux MBR
# boot code and syslinux itself for BIOS machines, and systemd-boot as
# EFI/BOOT/BOOTX64.EFI for UEFI machines (the removable-media path: no
# bootctl install, no NVRAM variable). Both boot loaders render their kernel
# command line from the same kernel-params file.
#
# NETBOOT_DIR  holds arch/boot/x86_64/vmlinuz-linux-rt,
#              arch/boot/x86_64/initramfs-linux-rt.img and
#              arch/x86_64/airootfs.sfs, copied as they are: the squashfs on
#              the stick is the very file a network boot serves.
# USB_DIR      holds kernel-params (the one source of the kernel command line),
#              syslinux.cfg.in, loader.conf and crtbridge.conf.in.
# OUT_IMG      the disk image to write.
# STICK_DIR    optional: files for the user, copied as they are to the root
#              of the stick (settings.ini, README.txt, examples/).
#
# Needs no root and no loop device: mkfs.fat, mtools, syslinux (its mtools
# installer), sfdisk and dd only. Runs in the pinned build container.
#
# Environment: SOURCE_DATE_EPOCH (required) gives the FAT serial number, so
# the same inputs give the same volume id; CRT_BRIDGE_VERSION (default
# unknown) is written to VERSION.txt at the root of the stick.
#
# Prints the UUID the kernel command line names, as `archisodevice=UUID=...`.
set -euo pipefail

die() { printf 'make-usb-img: %s\n' "$*" >&2; exit 1; }

[ "$#" -eq 3 ] || [ "$#" -eq 4 ] || die "usage: make-usb-img.sh NETBOOT_DIR USB_DIR OUT_IMG [STICK_DIR]"
NB=$1
USB=$2
OUT=$3
STICK=${4:-}
[ -z "$STICK" ] || [ -d "$STICK" ] || die "no directory at $STICK"
: "${SOURCE_DATE_EPOCH:?make-usb-img: SOURCE_DATE_EPOCH is required}"
case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) die "SOURCE_DATE_EPOCH must be a decimal number" ;;
esac
VERSION="${CRT_BRIDGE_VERSION:-unknown}"

KERNEL="$NB/arch/boot/x86_64/vmlinuz-linux-rt"
INITRD="$NB/arch/boot/x86_64/initramfs-linux-rt.img"
SQUASHFS="$NB/arch/x86_64/airootfs.sfs"
MBR_BIN=/usr/lib/syslinux/bios/mbr.bin
SDBOOT_EFI=/usr/lib/systemd/boot/efi/systemd-bootx64.efi
for f in "$KERNEL" "$INITRD" "$SQUASHFS" "$USB/kernel-params" \
         "$USB/syslinux.cfg.in" "$USB/loader.conf" "$USB/crtbridge.conf.in" \
         "$MBR_BIN" "$SDBOOT_EFI"; do
    [ -s "$f" ] || die "missing input: $f"
done

# The FAT serial number: the low 32 bits of SOURCE_DATE_EPOCH, shown by blkid
# and /dev/disk/by-uuid as XXXX-XXXX.
SERIAL=$(printf '%08X' "$(( SOURCE_DATE_EPOCH & 0xFFFFFFFF ))")
UUID="${SERIAL:0:4}-${SERIAL:4:4}"
START=2048   # 1 MiB, in 512-byte sectors

# One line, no template marker: a broken kernel-params file stops here, not
# at boot time on a screen nobody can read.
params=$(tr -d '\r' < "$USB/kernel-params")
[ "$(printf '%s\n' "$params" | grep -c .)" -eq 1 ] || die "kernel-params must hold exactly one line"
if printf '%s\n' "$params" | grep -q '@[A-Z_]*@'; then die "kernel-params holds a template marker"; fi
# sed renders the templates below: its delimiter and its match token must not
# appear in the line.
case "$params" in *'|'*|*'&'*) die "kernel-params holds a '|' or '&'" ;; esac

render() {
    sed -e "s|@UUID@|$UUID|g" -e "s|@KERNEL_PARAMS@|$params|g" "$1" > "$2"
    if grep -q '@[A-Z_]*@' "$2"; then die "unrendered marker left in $2"; fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TREE="$TMP/tree"
mkdir -p "$TREE/arch/boot/x86_64" "$TREE/arch/x86_64" "$TREE/syslinux" \
         "$TREE/EFI/BOOT" "$TREE/loader/entries"
cp "$KERNEL" "$INITRD" "$TREE/arch/boot/x86_64/"
cp "$SQUASHFS" "$TREE/arch/x86_64/"
render "$USB/syslinux.cfg.in" "$TREE/syslinux/syslinux.cfg"
cp "$SDBOOT_EFI" "$TREE/EFI/BOOT/BOOTX64.EFI"
cp "$USB/loader.conf" "$TREE/loader/loader.conf"
render "$USB/crtbridge.conf.in" "$TREE/loader/entries/crtbridge.conf"
printf '%s\n' "$VERSION" > "$TREE/VERSION.txt"
if [ -n "$STICK" ]; then
    for f in "$STICK"/*; do
        if [ -e "$TREE/$(basename "$f")" ]; then die "$(basename "$f") in $STICK would overwrite a boot file"; fi
    done
    cp -r "$STICK"/. "$TREE/"
fi
find "$TREE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

# Room for the boot loaders and for what the user adds later.
content_mib=$(du -s --block-size=1M "$TREE" | cut -f1)
part_mib=$(( content_mib + 64 ))
PART="$TMP/part.img"
export MTOOLS_SKIP_CHECK=1

# -h: the hidden-sector count, i.e. where the partition starts on the disk.
# A file system made in a separate file cannot guess it, and the syslinux boot
# sector needs it to find ldlinux.sys.
mkfs.fat -F 32 -n CRTBRIDGE -i "$SERIAL" -h "$START" -C "$PART" $(( part_mib * 1024 )) >/dev/null \
    || die "mkfs.fat failed"
mcopy -i "$PART" -s -p -m "$TREE"/* ::/ || die "mcopy failed"
syslinux --install --directory /syslinux "$PART" || die "syslinux --install failed"

rm -f "$OUT"
truncate -s $(( (part_mib + 1) * 1024 * 1024 )) "$OUT"
printf 'label: dos\nlabel-id: 0x%s\nunit: sectors\n\nstart=%d, size=%d, type=c, bootable\n' \
    "$SERIAL" "$START" $(( part_mib * 2048 )) \
    | sfdisk --no-reread --no-tell-kernel --quiet "$OUT" || die "sfdisk failed"
dd if="$PART" of="$OUT" bs=1M seek=1 conv=notrunc status=none || die "dd of the partition failed"
dd if="$MBR_BIN" of="$OUT" bs=440 count=1 conv=notrunc status=none || die "dd of mbr.bin failed"

printf 'archisodevice=UUID=%s\n' "$UUID"
