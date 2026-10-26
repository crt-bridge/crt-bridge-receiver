#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build-image.sh -- builds the crt-bridge receiver image from source.
#
#   SOURCE_DATE_EPOCH=<seconds> build-image.sh SRC_ROOT OUT_DIR
#   SOURCE_DATE_EPOCH=<seconds> build-image.sh --repack NETBOOT_DIR SRC_ROOT OUT_DIR
#
# SRC_ROOT is the root of the published receiver tree (src/, tools/,
# systemd/, image/, hardware/). OUT_DIR is created if needed and receives:
#
#   netboot/arch/boot/x86_64/vmlinuz-linux-rt     the three network-boot
#   netboot/arch/boot/x86_64/initramfs-linux-rt.img   artefacts
#   netboot/arch/x86_64/airootfs.sfs
#   crt-bridge-receiver-<version>.img   the USB stick image (same squashfs)
#   packages.txt      every package in the image (pacman -Q)
#   build-info.txt    the pins and the checksums of this build
#   SHA256SUMS        checksums of the above
#   logs/             the build logs
#
# Every step runs in the Arch Linux container pinned by digest in
# image/build-env, with every package taken from one dated Arch Linux Archive
# snapshot and checked against its signature. switchres is built from its
# pinned commit with libdrm only. Needs: bash, coreutils, sed, grep and a
# working docker (the image step runs a --privileged container, as mkarchiso
# needs to mount inside its chroot). Nothing is published anywhere.
#
# --repack makes only the disk image again, from the netboot artefacts of an
# earlier build (NETBOOT_DIR is that build's netboot/ directory): the squashfs
# does not change by a single byte.
#
# Environment:
#   SOURCE_DATE_EPOCH    required: build timestamp, and the FAT serial number
#   CRT_BRIDGE_VERSION   version name written to the stick (default dev)
#   CRT_BRIDGE_SOURCE    source revision recorded in the image (default unknown)
set -euo pipefail

die() { printf 'build-image: ERROR: %s\n' "$*" >&2; exit 1; }
step() { printf '\n=== build-image: %s ===\n' "$*"; }

# ---------------------------------------------------------------------------
# Pins
# ---------------------------------------------------------------------------

# Host side: read from build-env by read_pins. Inside the container: passed
# in the environment by run_inside, never read again.
read_pins() {
    local file=$1 line key val
    [ -r "$file" ] || die "cannot read $file"
    ARCH_IMAGE="" ALA_SNAPSHOT="" SWITCHRES_URL="" SWITCHRES_COMMIT="" SMOKE_IMAGE=""
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%$'\r'}
        case "$line" in ''|'#'*) continue ;; esac
        key=${line%%=*}
        val=${line#*=}
        case "$key" in
            ARCH_IMAGE) ARCH_IMAGE=$val ;;
            ALA_SNAPSHOT) ALA_SNAPSHOT=$val ;;
            SWITCHRES_URL) SWITCHRES_URL=$val ;;
            SWITCHRES_COMMIT) SWITCHRES_COMMIT=$val ;;
            SMOKE_IMAGE) SMOKE_IMAGE=$val ;;
            *) die "unknown key in $file: $key" ;;
        esac
    done < "$file"
    [[ $ARCH_IMAGE =~ ^archlinux@sha256:[0-9a-f]{64}$ ]] || die "ARCH_IMAGE must be archlinux@sha256:<64 hex>"
    [[ $ALA_SNAPSHOT =~ ^[0-9]{4}/[0-9]{2}/[0-9]{2}$ ]] || die "ALA_SNAPSHOT must be YYYY/MM/DD"
    [[ $SWITCHRES_COMMIT =~ ^[0-9a-f]{40}$ ]] || die "SWITCHRES_COMMIT must be a full commit id"
    [[ $SWITCHRES_URL =~ ^https://[A-Za-z0-9./_-]+$ ]] || die "SWITCHRES_URL must be an https URL"
}

# ---------------------------------------------------------------------------
# Steps run INSIDE the pinned container (this script, mounted read-only)
# ---------------------------------------------------------------------------

inside_pacman() {
    # The archive line is the only server: the container's mirror list is
    # replaced, never appended to.
    printf 'Server = https://archive.archlinux.org/repos/%s/$repo/os/$arch\n' "$ALA_SNAPSHOT" \
        > /etc/pacman.d/mirrorlist
    if ! pacman -Syyuu --noconfirm --needed "$@" > /tmp/pacman.log 2>&1; then
        tail -30 /tmp/pacman.log >&2
        die "pacman -Syyuu failed (snapshot $ALA_SNAPSHOT)"
    fi
}

inside_daemon() {
    inside_pacman make gcc git pkgconf libdrm lz4 linux-api-headers
    # switchres compiles in X and SDL2 support when it finds them: their
    # absence keeps libswitchres.a the same for everyone.
    for p in sdl2 sdl2-compat libxrandr; do
        if pacman -Qq "$p" >/dev/null 2>&1; then die "$p is installed: switchres would link it"; fi
    done

    git clone --quiet "$SWITCHRES_URL" /tmp/switchres || die "git clone of switchres failed"
    git -C /tmp/switchres -c advice.detachedHead=false checkout --quiet "$SWITCHRES_COMMIT" \
        || die "switchres commit $SWITCHRES_COMMIT not found"
    head=$(git -C /tmp/switchres rev-parse HEAD)
    [ "$head" = "$SWITCHRES_COMMIT" ] || die "switchres HEAD is $head, expected $SWITCHRES_COMMIT"
    if ! make -C /tmp/switchres libswitchres > /work/logs/switchres.log 2>&1; then
        tail -30 /work/logs/switchres.log >&2
        die "switchres build failed"
    fi
    grep -q 'KMS support enabled' /work/logs/switchres.log || die "switchres built without KMS support"
    [ -s /tmp/switchres/libswitchres.a ] || die "libswitchres.a missing"

    mkdir -p /tmp/b
    cp -r /src/src /src/tools /tmp/b/
    if ! make -C /tmp/b/src SWITCHRES_DIR=/tmp/switchres HDA_SRC=/tmp/b/tools/hda-volume.c \
            > /work/logs/daemon.log 2>&1; then
        tail -40 /work/logs/daemon.log >&2
        die "daemon build failed"
    fi
    rcv=/tmp/b/src/crt-bridge-rcv
    vol=/tmp/b/src/hda-volume
    cfg=/tmp/b/src/crt-bridge-config
    [ -x "$rcv" ] && [ -x "$vol" ] && [ -x "$cfg" ] || die "daemon, hda-volume or crt-bridge-config missing after make"
    # ldd to a file first: under pipefail, grep -q closing the pipe early
    # could hide a match.
    ldd "$rcv" > /work/logs/daemon-ldd.txt 2>&1 || true
    if grep -q 'not found' /work/logs/daemon-ldd.txt; then die "unresolved dynamic dependency in the daemon"; fi
    if grep -q libswitchres /work/logs/daemon-ldd.txt; then die "daemon links switchres dynamically, static expected"; fi
    "$rcv" --version > /work/logs/daemon-version.txt 2>&1 || die "daemon --version failed"
    "$vol" --help > /dev/null 2>&1 || die "hda-volume --help failed"
    cfg_rc=0
    "$cfg" > /dev/null 2>&1 || cfg_rc=$?
    [ "$cfg_rc" -eq 2 ] || die "crt-bridge-config without options exits $cfg_rc, 2 (usage) expected"

    install -d /work/bin
    install -m 0755 "$rcv" "$vol" "$cfg" /work/bin/
    {
        printf 'switchres_head=%s\n' "$head"
        printf 'gcc=%s\n' "$(gcc --version | head -n 1)"
    } > /work/bin/toolchain.txt
    chown -R "$HOST_UID:$HOST_GID" /work/bin /work/logs
}

inside_mkarchiso() {
    inside_pacman archiso
    rm -rf /tmp/aw /tmp/ao
    if ! mkarchiso -v -w /tmp/aw -o /tmp/ao /profile > /work/logs/mkarchiso.log 2>&1; then
        tail -60 /work/logs/mkarchiso.log >&2
        die "mkarchiso failed"
    fi
    install -d /work/netboot/arch/boot/x86_64 /work/netboot/arch/x86_64
    for f in arch/boot/x86_64/vmlinuz-linux-rt arch/boot/x86_64/initramfs-linux-rt.img \
             arch/x86_64/airootfs.sfs; do
        [ -s "/tmp/ao/$f" ] || die "mkarchiso produced no $f"
        cp "/tmp/ao/$f" "/work/netboot/$f"
    done
    [ -d /tmp/aw/x86_64/airootfs/var/lib/pacman ] || die "built root not found in the mkarchiso work directory"
    pacman -Q --sysroot /tmp/aw/x86_64/airootfs > /work/packages.txt || die "pacman -Q on the built root failed"
    chown -R "$HOST_UID:$HOST_GID" /work/netboot /work/packages.txt /work/logs
}

inside_pack() {
    inside_pacman syslinux mtools dosfstools util-linux systemd
    # What the user finds at the root of the stick. The two text files are
    # read under Windows: they go there with CRLF line endings (settings.ini
    # is read back through crt-bridge-config, which accepts them). The example
    # tube file stays byte for byte the file of hardware/monitors/.
    rm -rf /tmp/stick
    mkdir -p /tmp/stick/examples
    for f in settings.ini README.txt; do
        sed 's/$/\r/' "/src/image/usb/$f" > "/tmp/stick/$f" || die "cannot stage $f for the stick"
    done
    cp /src/hardware/monitors/jvc_dtv1710cg.ini /tmp/stick/examples/monitor-jvc_dtv1710cg.ini
    bash /src/image/usb/make-usb-img.sh /work/netboot /src/image/usb "/work/$IMG_NAME" /tmp/stick \
        > /work/logs/usb-uuid.txt || die "make-usb-img.sh failed"
    chown "$HOST_UID:$HOST_GID" "/work/$IMG_NAME" /work/logs/usb-uuid.txt
}

if [ "${1:-}" = "--inside" ]; then
    # Whatever happens, hand the shared work directory back to the caller:
    # root-owned leftovers would block the next run.
    trap 'chown -R "$HOST_UID:$HOST_GID" /work 2>/dev/null || true' EXIT
    case "${2:-}" in
        daemon) inside_daemon ;;
        mkarchiso) inside_mkarchiso ;;
        pack) inside_pack ;;
        *) die "unknown inside step: ${2:-}" ;;
    esac
    exit 0
fi

# ---------------------------------------------------------------------------
# Host side
# ---------------------------------------------------------------------------

REPACK=""
if [ "${1:-}" = "--repack" ]; then
    [ "$#" -eq 4 ] || die "usage: build-image.sh --repack NETBOOT_DIR SRC_ROOT OUT_DIR"
    REPACK=$(cd "$2" && pwd) || die "cannot enter $2"
    shift 2
fi
[ "$#" -eq 2 ] || die "usage: build-image.sh SRC_ROOT OUT_DIR (or --repack NETBOOT_DIR SRC_ROOT OUT_DIR)"
SRC=$(cd "$1" && pwd) || die "cannot enter $1"
mkdir -p "$2"
OUT=$(cd "$2" && pwd)

: "${SOURCE_DATE_EPOCH:?build-image: SOURCE_DATE_EPOCH is required}"
[[ $SOURCE_DATE_EPOCH =~ ^[0-9]+$ ]] || die "SOURCE_DATE_EPOCH must be a decimal number"
VERSION="${CRT_BRIDGE_VERSION:-dev}"
SOURCE_REV="${CRT_BRIDGE_SOURCE:-unknown}"
[[ $VERSION =~ ^[A-Za-z0-9._-]+$ ]] || die "CRT_BRIDGE_VERSION may hold only letters, digits, '.', '_' and '-'"
[[ $SOURCE_REV =~ ^[A-Za-z0-9._-]+$ ]] || die "CRT_BRIDGE_SOURCE may hold only letters, digits, '.', '_' and '-'"
IMG_NAME="crt-bridge-receiver-$VERSION.img"
export SOURCE_DATE_EPOCH

command -v docker >/dev/null || die "docker not found"
read_pins "$SRC/image/build-env"

PROFILE_SRC="$SRC/image/archiso-profile"
grep -q "^Server = https://archive.archlinux.org/repos/$ALA_SNAPSHOT/" "$PROFILE_SRC/pacman.conf" \
    || die "archiso-profile/pacman.conf does not name the snapshot $ALA_SNAPSHOT of build-env"
# Package signatures stay required: one SigLevel line, Arch's default, and no
# repository may override it.
siglevels=$(grep -c '^[[:space:]]*SigLevel' "$PROFILE_SRC/pacman.conf" || true)
grep -qx 'SigLevel = Required DatabaseOptional' "$PROFILE_SRC/pacman.conf" && [ "$siglevels" -eq 1 ] \
    || die "archiso-profile/pacman.conf must hold exactly one SigLevel line: SigLevel = Required DatabaseOptional"

WORK="$OUT/work"
rm -rf "$WORK"
mkdir -p "$WORK/logs" "$OUT/logs"

run_inside() {
    local stepname=$1; shift
    docker run --rm "$@" \
        -e ALA_SNAPSHOT="$ALA_SNAPSHOT" \
        -e SWITCHRES_URL="$SWITCHRES_URL" \
        -e SWITCHRES_COMMIT="$SWITCHRES_COMMIT" \
        -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
        -e CRT_BRIDGE_VERSION="$VERSION" \
        -e IMG_NAME="$IMG_NAME" \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        -v "$SRC:/src:ro" \
        -v "$WORK:/work" \
        "$ARCH_IMAGE" bash /src/image/build-image.sh --inside "$stepname"
}

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

if [ -z "$REPACK" ]; then
    step "pre-flight: display files"
    EDID="$SRC/hardware/edid/jvc_dtv1710cg.bin"
    EDID_SHA256=f97d52f5edd4466e0dc4d4a9b2d9a4cfd6eb2681b38d41afd377fb1862986fd6
    MONITOR="$SRC/hardware/monitors/jvc_dtv1710cg.ini"
    [ "$(sha_of "$EDID")" = "$EDID_SHA256" ] || die "EDID checksum mismatch: $EDID"
    [ "$(wc -c < "$EDID")" -eq 128 ] || die "EDID must be 128 bytes"
    # CR bytes counted in raw bytes: the daemon refuses a monitor file that
    # carries one (exit code 3 at every boot).
    for f in "$MONITOR" "$PROFILE_SRC/airootfs/etc/crt-bridge/rcv.env"; do
        [ -f "$f" ] || die "missing $f"
        [ "$(tr -cd '\r' < "$f" | wc -c)" -eq 0 ] || die "$f carries CR bytes"
    done
    # The stick's text files get their CRLF at packing time: a CR already
    # there would be doubled.
    for f in "$SRC/image/usb/settings.ini" "$SRC/image/usb/README.txt"; do
        [ -f "$f" ] || die "missing $f"
        [ "$(tr -cd '\r' < "$f" | wc -c)" -eq 0 ] || die "$f carries CR bytes"
    done

    step "daemon, hda-volume and crt-bridge-config (switchres $SWITCHRES_COMMIT)"
    run_inside daemon || die "daemon step failed"
    rcv_sha=$(sha_of "$WORK/bin/crt-bridge-rcv")
    vol_sha=$(sha_of "$WORK/bin/hda-volume")
    cfg_sha=$(sha_of "$WORK/bin/crt-bridge-config")
    printf 'daemon            sha256 = %s\n' "$rcv_sha"
    printf 'hda-volume        sha256 = %s\n' "$vol_sha"
    printf 'crt-bridge-config sha256 = %s\n' "$cfg_sha"

    step "profile staging"
    P="$WORK/profile"
    cp -r "$PROFILE_SRC" "$P"
    A="$P/airootfs"
    install -d "$A/usr/lib/firmware/edid" "$A/etc/crt-bridge" "$A/usr/local/bin" \
               "$A/etc/systemd/system" "$A/root/customize_airootfs.d" \
               "$A/usr/share/crt-bridge/monitors"
    cp "$EDID" "$A/usr/lib/firmware/edid/jvc_dtv1710cg.bin"
    # The bench tube's file, built in for the developer mode only: the public
    # default passes no monitor file (the built-in 15 kHz band), and a user
    # puts monitor.ini on the stick.
    cp "$MONITOR" "$A/usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini"
    cp "$WORK/bin/crt-bridge-rcv" "$WORK/bin/hda-volume" "$WORK/bin/crt-bridge-config" "$A/usr/local/bin/"
    chmod 0755 "$A/usr/local/bin/crt-bridge-rcv" "$A/usr/local/bin/hda-volume" "$A/usr/local/bin/crt-bridge-config"
    cp "$SRC/systemd/"*.service "$A/etc/systemd/system/"
    cp "$SRC/image/customize_airootfs.d/"*.sh "$A/root/customize_airootfs.d/"
    # The version name is NOT recorded here: it lives on the stick only, so
    # naming a release late never rebuilds the squashfs.
    {
        printf 'source=%s\n' "$SOURCE_REV"
        printf 'ala_snapshot=%s\n' "$ALA_SNAPSHOT"
        printf 'switchres_commit=%s\n' "$SWITCHRES_COMMIT"
        printf 'source_date_epoch=%s\n' "$SOURCE_DATE_EPOCH"
    } > "$A/etc/crt-bridge/build-info"

    # Checksums after the copy: a silently truncated copy would give an image
    # that boots and does nothing.
    [ "$(sha_of "$A/usr/lib/firmware/edid/jvc_dtv1710cg.bin")" = "$EDID_SHA256" ] || die "EDID changed during the copy"
    [ "$(sha_of "$A/usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini")" = "$(sha_of "$MONITOR")" ] || die "the monitor file changed during the copy"
    [ "$(sha_of "$A/usr/local/bin/crt-bridge-rcv")" = "$rcv_sha" ] || die "daemon changed during the copy"
    [ "$(sha_of "$A/usr/local/bin/hda-volume")" = "$vol_sha" ] || die "hda-volume changed during the copy"
    [ "$(sha_of "$A/usr/local/bin/crt-bridge-config")" = "$cfg_sha" ] || die "crt-bridge-config changed during the copy"
    for f in usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini etc/crt-bridge/rcv.env; do
        [ "$(tr -cd '\r' < "$A/$f" | wc -c)" -eq 0 ] || die "$f carries CR bytes in the staged profile"
    done
    for f in \
        usr/local/bin/crt-bridge-rcv \
        usr/local/bin/hda-volume \
        usr/local/bin/crt-bridge-config \
        usr/local/lib/crt-bridge/write-report \
        usr/local/lib/crt-bridge/read-stick \
        etc/systemd/system/crt-bridge-rcv.service \
        etc/systemd/system/crt-bridge-volume.service \
        etc/systemd/system/crt-bridge-wol.service \
        etc/systemd/system/crt-bridge-report.service \
        etc/systemd/system/crt-bridge-config.service \
        etc/crt-bridge/rcv.env \
        usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini \
        etc/crt-bridge/build-info \
        etc/systemd/network/10-eth0-wol.link \
        root/customize_airootfs.d/30-crt-bridge-rcv.sh
    do
        [ -e "$A/$f" ] || die "staged profile lacks $f"
    done
    if [ -e "$A/root/.ssh" ]; then die "staged profile carries /root/.ssh: no key ever goes into the image"; fi

    step "squashfs (mkarchiso, netboot mode)"
    docker run --rm --privileged \
        -e ALA_SNAPSHOT="$ALA_SNAPSHOT" \
        -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        -v "$SRC:/src:ro" \
        -v "$P:/profile:ro" \
        -v "$WORK:/work" \
        "$ARCH_IMAGE" bash /src/image/build-image.sh --inside mkarchiso \
        || die "mkarchiso step failed"
    kernel_pkg=$(sed -n 's/^linux-rt //p' "$WORK/packages.txt")
    [ -n "$kernel_pkg" ] || die "linux-rt is not in the built image"
    printf 'kernel package: linux-rt %s\n' "$kernel_pkg"
else
    step "repack from $REPACK"
    for f in arch/boot/x86_64/vmlinuz-linux-rt arch/boot/x86_64/initramfs-linux-rt.img arch/x86_64/airootfs.sfs; do
        [ -s "$REPACK/$f" ] || die "missing $REPACK/$f"
        install -D -m 0644 "$REPACK/$f" "$WORK/netboot/$f"
    done
    PREV_INFO="$REPACK/../build-info.txt"
    if [ -r "$PREV_INFO" ]; then
        prev_sfs=$(sed -n 's/^squashfs_sha256=//p' "$PREV_INFO")
        [ "$prev_sfs" = "$(sha_of "$WORK/netboot/arch/x86_64/airootfs.sfs")" ] \
            || die "the squashfs in $REPACK is not the one $PREV_INFO records"
        [ -r "$REPACK/../packages.txt" ] && cp "$REPACK/../packages.txt" "$WORK/packages.txt"
    fi
fi

step "disk image"
run_inside pack || die "pack step failed"
uuid_line=$(cat "$WORK/logs/usb-uuid.txt")
printf '%s\n' "$uuid_line"

step "outputs"
rm -rf "$OUT/netboot"
mv "$WORK/netboot" "$OUT/netboot"
mv "$WORK/$IMG_NAME" "$OUT/$IMG_NAME"
[ -f "$WORK/packages.txt" ] && mv "$WORK/packages.txt" "$OUT/packages.txt"
cp "$WORK/logs/"* "$OUT/logs/"

sfs_sha=$(sha_of "$OUT/netboot/arch/x86_64/airootfs.sfs")
img_sha=$(sha_of "$OUT/$IMG_NAME")
if [ -z "$REPACK" ]; then
    {
        printf 'arch_image=%s\n' "$ARCH_IMAGE"
        printf 'ala_snapshot=%s\n' "$ALA_SNAPSHOT"
        printf 'switchres_commit=%s\n' "$SWITCHRES_COMMIT"
        printf 'kernel_package=linux-rt %s\n' "$kernel_pkg"
        printf 'daemon_sha256=%s\n' "$rcv_sha"
        printf 'hda_volume_sha256=%s\n' "$vol_sha"
        printf 'crt_bridge_config_sha256=%s\n' "$cfg_sha"
        printf 'squashfs_sha256=%s\n' "$sfs_sha"
        printf 'img_name=%s\n' "$IMG_NAME"
        printf 'img_sha256=%s\n' "$img_sha"
        printf 'fat_%s\n' "${uuid_line#archisodevice=}"
        printf 'version=%s\n' "$VERSION"
        printf 'source_date_epoch=%s\n' "$SOURCE_DATE_EPOCH"
        printf 'source=%s\n' "$SOURCE_REV"
    } > "$OUT/build-info.txt"
else
    {
        if [ -r "$PREV_INFO" ]; then
            grep -v -e '^img_name=' -e '^img_sha256=' -e '^fat_UUID=' -e '^version=' -e '^repacked=' "$PREV_INFO"
        fi
        printf 'img_name=%s\n' "$IMG_NAME"
        printf 'img_sha256=%s\n' "$img_sha"
        printf 'fat_%s\n' "${uuid_line#archisodevice=}"
        printf 'version=%s\n' "$VERSION"
        printf 'repacked=yes\n'
    } > "$OUT/build-info.next"
    mv "$OUT/build-info.next" "$OUT/build-info.txt"
fi

(
    cd "$OUT"
    files=("$IMG_NAME" netboot/arch/boot/x86_64/vmlinuz-linux-rt
           netboot/arch/boot/x86_64/initramfs-linux-rt.img netboot/arch/x86_64/airootfs.sfs)
    [ -f packages.txt ] && files+=(packages.txt)
    sha256sum "${files[@]}" > SHA256SUMS
)
rm -rf "$WORK"

step "done"
cat "$OUT/build-info.txt"
