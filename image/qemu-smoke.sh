#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# qemu-smoke.sh -- boots a receiver stick image under QEMU and reads back the
# report the image writes on its own FAT partition.
#
#   qemu-smoke.sh bios|uefi IMG OUTDIR [--append ARGS] [--tag T] [--timeout S]
#                 [--net dhcp|none] [--put NAME=FILE]... [--smp N]
#                 [--ssh-probe] [--ssh-test-key] [--crlf-boot]
#
# --ssh-probe tries port 22 of the guest (forwarded to port 2222 of the
# container) after the report signal and before the shutdown, and writes to
# smoke-info.txt:
#   ssh_probe: refused   nothing accepted the connection (no forwarded port);
#   ssh_probe: closed    QEMU accepted it, but the guest closed it without a
#                        word: nothing listens on port 22;
#   ssh_probe: open      the guest answered with an SSH banner.
# When the port is open, a login without any key is tried as well
# (ssh_nokey_login: denied|ok, and the methods the server offered).
#
# --ssh-test-key creates a throwaway ed25519 key pair inside the container,
# for this boot only, and removes it before the container exits: no key is
# ever written outside the container. The word @SSHKEY@ in --append becomes
# the base64 part of its public key, and once the port is open, a login with
# it is tried (ssh_login: ok|denied). The public part is replaced by
# "<test key>" in smoke-info.txt and serial.log.
#
# --crlf-boot rewrites syslinux/syslinux.cfg and loader/entries/crtbridge.conf
# of the working copy with CRLF line ends, as a Windows editor may save them,
# after the extra arguments are appended.
#
# --smp gives the machine N logical CPUs (1 to 16, 4 by default).
#
# --put copies FILE to the root of the stick's FAT, as NAME, in the working
# copy and before the boot: a monitor.ini or a settings.ini as a user would
# leave it there. It may be given several times.
#
# bios boots through SeaBIOS (syslinux), uefi through OVMF (systemd-boot from
# EFI/BOOT/BOOTX64.EFI). The stick is a removable USB disk behind an xHCI
# controller, as on real hardware; 4 GiB of memory, 4 logical CPUs unless
# --smp says otherwise.
#
# The network card (e1000) is on QEMU's user network by default
# (--net dhcp), which answers DHCP. With --net none the card is plugged into
# a network where nobody answers: its link is up, its frames go to a UDP
# port of the container where nothing listens, and no lease ever comes.
#
# IMG is never modified: the boot runs on a working copy, OUTDIR/stick.img.
# console=ttyS0,115200 and ARGS are appended to the kernel command line of
# that copy only. The script waits for the image's own signal on the serial
# console ("crt-bridge-report: written", at most S seconds, 900 by default),
# powers the machine down through the QEMU monitor, then leaves in OUTDIR:
#
#   report.txt      the report the image wrote on the stick
#   user-env.txt    the environment the daemon was given, as the report
#                   quotes /run/crt-bridge/user.env (no secret is in it)
#   mdir.txt        the stick's file tree after the boot (mdir -/)
#   fat-squashfs.sha256  checksum of the squashfs read back from the stick
#   serial.log      the whole serial console
#   smoke-info.txt  mode, result, the acceleration really used (kvm or tcg)
#                   and the QEMU command line
#   screen.ppm      a screenshot, only when the signal never came
#   stick.img       the working copy, after shutdown
#
# QEMU runs in the Debian container pinned by SMOKE_IMAGE in build-env, with
# /dev/kvm when the host offers it (else TCG, much slower). QEMU proves the
# boot chain, not the display: there is no Radeon card in a virtual machine.
#
# Exit status: 0 when the report was written and read back, 1 otherwise.
set -euo pipefail

die() { printf 'qemu-smoke: ERROR: %s\n' "$*" >&2; exit 2; }

PART_OFFSET=1048576   # the FAT partition starts at sector 2048

# ---------------------------------------------------------------------------
# Inside the container
# ---------------------------------------------------------------------------

append_to_line() {
    # append_to_line FAT_PATH KEYWORD: append $EXTRA to the line starting with
    # KEYWORD in that file of the working copy.
    local fat_path=$1 keyword=$2 tmp
    tmp=$(mktemp)
    mcopy -n -i "/work/stick.img@@$PART_OFFSET" "::$fat_path" "$tmp" 2>/dev/null || return 0
    sed -i -e 's/\r$//' -e "/^[[:space:]]*$keyword /s|\$| $EXTRA|" "$tmp"
    if [ "$CRLF_BOOT" = 1 ]; then
        sed -i -e 's/$/\r/' "$tmp"
    fi
    mcopy -o -i "/work/stick.img@@$PART_OFFSET" "$tmp" "::$fat_path"
    rm -f "$tmp"
}

cr_count() {
    # cr_count FAT_PATH: carriage returns in that file of the working copy.
    { mcopy -n -i "/work/stick.img@@$PART_OFFSET" "::$1" - 2>/dev/null || true; } | tr -cd '\r' | wc -c
}

ssh_opts() {
    printf '%s\n' -p 2222 -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
}

ssh_checks() {
    # Port 22 of the guest, through QEMU's forwarded port. A plain TCP connect
    # is not enough: QEMU accepts on 2222 whatever the guest does, then
    # closes when the guest refuses. Only an SSH banner means "open".
    local line="" err
    rm -f /tmp/banner
    err=$(timeout 15 bash -c 'IFS= read -r -t 10 line < /dev/tcp/127.0.0.1/2222; printf "%s" "$line" > /tmp/banner' 2>&1 || true)
    line=$(cat /tmp/banner 2>/dev/null || true)
    if printf '%s' "$err" | grep -qi 'refused'; then
        SSH_PROBE_RESULT=refused
    elif [[ $line == SSH-* ]]; then
        SSH_PROBE_RESULT=open
        SSH_BANNER=$(printf '%s' "$line" | tr -cd '[:print:]')
    else
        SSH_PROBE_RESULT=closed
    fi
    [ "$SSH_PROBE_RESULT" = open ] || return 0

    local out opts
    mapfile -t opts < <(ssh_opts)
    # No key at all: the server must say no, and name the methods it offers.
    out=$(timeout 30 ssh "${opts[@]}" -o PubkeyAuthentication=no root@127.0.0.1 true 2>&1) \
        && SSH_NOKEY=ok || SSH_NOKEY=denied
    SSH_NOKEY_METHODS=$(printf '%s' "$out" | sed -n 's/.*Permission denied (\([^)]*\)).*/\1/p' | head -n 1)
    if [ "$SSH_TEST_KEY" = 1 ]; then
        out=$(timeout 30 ssh "${opts[@]}" -o IdentitiesOnly=yes -i /tmp/smoke-key/id root@127.0.0.1 \
            'printf "crtbridge-login uid=%s\n" "$(id -u)"' 2>&1 || true)
        if printf '%s\n' "$out" | grep -qx 'crtbridge-login uid=0'; then SSH_LOGIN=ok; else SSH_LOGIN=denied; fi
    fi
}

inside() {
    trap 'chown -R "$HOST_UID:$HOST_GID" /work 2>/dev/null || true' EXIT
    export MTOOLS_SKIP_CHECK=1
    EXTRA="console=ttyS0,115200${APPEND_ARGS:+ $APPEND_ARGS}"
    SSH_PROBE_RESULT=not-run; SSH_BANNER=""; SSH_NOKEY=not-tried; SSH_NOKEY_METHODS=""; SSH_LOGIN=not-tried
    local blob="" key_state=none
    if [ "$SSH_TEST_KEY" = 1 ]; then
        # A throwaway key for this boot only, in the container's /tmp.
        rm -rf /tmp/smoke-key
        install -d -m 0700 /tmp/smoke-key
        ssh-keygen -q -t ed25519 -N '' -C crtbridge-smoke -f /tmp/smoke-key/id
        blob=$(cut -d' ' -f2 /tmp/smoke-key/id.pub)
        EXTRA=${EXTRA//@SSHKEY@/$blob}
        key_state=created
    fi
    local shown_extra=$EXTRA
    [ -n "$blob" ] && shown_extra=${EXTRA//$blob/<test key>}
    append_to_line syslinux/syslinux.cfg APPEND
    append_to_line loader/entries/crtbridge.conf options
    local cr_syslinux cr_loader
    cr_syslinux=$(cr_count syslinux/syslinux.cfg)
    cr_loader=$(cr_count loader/entries/crtbridge.conf)
    local name
    for name in $PUT_NAMES; do
        mcopy -o -i "/work/stick.img@@$PART_OFFSET" "/work/put/$name" "::$name" \
            || { echo "qemu-smoke: cannot put $name on the stick" >&2; return 1; }
    done

    local accel cpu
    if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then accel=kvm; cpu=host; else accel=tcg; cpu=max; fi

    local cmd=(qemu-system-x86_64 -machine "q35,accel=$accel" -cpu "$cpu" -m 4096 -smp "$SMP")
    if [ "$MODE" = uefi ]; then
        cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/vars.fd
        cmd+=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
              -drive if=pflash,format=raw,file=/tmp/vars.fd)
    fi
    cmd+=(-drive if=none,id=stick,format=raw,file=/work/stick.img
          -device qemu-xhci -device usb-storage,drive=stick,removable=on,bootindex=1)
    if [ "$NET" = none ]; then
        cmd+=(-netdev socket,id=lan,udp=127.0.0.1:40002,localaddr=127.0.0.1:40001
              -device e1000,netdev=lan)
    else
        cmd+=(-nic user,model=e1000,hostfwd=tcp::2222-:22)
    fi
    cmd+=(-display none -serial file:/work/serial.log
          -monitor unix:/tmp/mon.sock,server,nowait -no-reboot)

    : > /work/serial.log
    local start now result=timeout
    start=$(date +%s)
    "${cmd[@]}" &
    local qpid=$!

    while :; do
        sleep 2
        now=$(date +%s)
        if grep -q 'crt-bridge-report: written' /work/serial.log; then result=written; break; fi
        if grep -q 'crt-bridge-report: failed' /work/serial.log; then result=report-failed; break; fi
        if grep -q 'crt-bridge-report: no boot stick' /work/serial.log; then result=no-stick; break; fi
        if ! kill -0 "$qpid" 2>/dev/null; then result=qemu-exited; break; fi
        if [ $(( now - start )) -ge "$TIMEOUT" ]; then result=timeout; break; fi
    done
    local signal_after=$(( $(date +%s) - start ))

    if [ "$SSH_PROBE" = 1 ] && kill -0 "$qpid" 2>/dev/null; then
        ssh_checks
    fi
    if [ "$SSH_TEST_KEY" = 1 ]; then
        rm -rf /tmp/smoke-key
        [ -e /tmp/smoke-key ] && key_state=NOT-REMOVED || key_state="created in the container, removed"
    fi

    local shutdown=none
    if kill -0 "$qpid" 2>/dev/null; then
        if [ "$result" = timeout ]; then
            echo "screendump /work/screen.ppm" | socat - UNIX-CONNECT:/tmp/mon.sock >/dev/null 2>&1 || true
            sleep 2
        fi
        echo system_powerdown | socat - UNIX-CONNECT:/tmp/mon.sock >/dev/null 2>&1 || true
        shutdown=powerdown
        for _ in $(seq 1 60); do
            kill -0 "$qpid" 2>/dev/null || break
            sleep 2
        done
        if kill -0 "$qpid" 2>/dev/null; then
            echo quit | socat - UNIX-CONNECT:/tmp/mon.sock >/dev/null 2>&1 || true
            shutdown=forced-quit
            sleep 2
        fi
    fi
    wait "$qpid" 2>/dev/null || true

    rm -f /work/report.txt
    mcopy -n -i "/work/stick.img@@$PART_OFFSET" ::report.txt /work/report.txt 2>/dev/null || true
    # The environment the daemon was given, as the report quotes it from
    # /run/crt-bridge/user.env ("user.env: " lines).
    sed -n 's/^user\.env: //p' /work/report.txt > /work/user-env.txt 2>/dev/null || true
    # What the stick holds after the boot, and the checksum of the squashfs
    # read back from the FAT (the very file a network boot serves).
    mdir -/ -i "/work/stick.img@@$PART_OFFSET" :: > /work/mdir.txt 2>&1 || true
    mcopy -n -i "/work/stick.img@@$PART_OFFSET" ::arch/x86_64/airootfs.sfs - 2>/dev/null \
        | sha256sum > /work/fat-squashfs.sha256 || true

    {
        printf 'mode=%s\n' "$MODE"
        printf 'tag=%s\n' "${TAG:-}"
        printf 'net=%s\n' "$NET"
        printf 'put=%s\n' "$PUT_NAMES"
        printf 'smp=%s\n' "$SMP"
        printf 'result=%s\n' "$result"
        printf 'signal_after_s=%s\n' "$signal_after"
        printf 'shutdown=%s\n' "$shutdown"
        printf 'accel=%s\n' "$accel"
        printf 'report_read=%s\n' "$([ -s /work/report.txt ] && echo yes || echo no)"
        printf 'qemu_version=%s\n' "$(qemu-system-x86_64 --version | head -n 1)"
        printf 'kernel_extra_args=%s\n' "$shown_extra"
        printf 'crlf_boot=%s\n' "$([ "$CRLF_BOOT" = 1 ] && echo yes || echo no)"
        printf 'cr_bytes=syslinux.cfg:%s crtbridge.conf:%s\n' "$cr_syslinux" "$cr_loader"
        printf 'ssh_test_key=%s\n' "$key_state"
        printf 'qemu_command=%s\n' "${cmd[*]}"
        if [ "$SSH_PROBE" = 1 ]; then
            printf 'ssh_probe: %s\n' "$SSH_PROBE_RESULT"
            [ -n "$SSH_BANNER" ] && printf 'ssh_banner: %s\n' "$SSH_BANNER"
            printf 'ssh_nokey_login: %s\n' "$SSH_NOKEY"
            [ -n "$SSH_NOKEY_METHODS" ] && printf 'ssh_nokey_methods: %s\n' "$SSH_NOKEY_METHODS"
            [ "$SSH_TEST_KEY" = 1 ] && printf 'ssh_login: %s\n' "$SSH_LOGIN"
        fi
    } > /work/smoke-info.txt
    if [ -n "$blob" ]; then
        sed -i "s|$blob|<test key>|g" /work/serial.log /work/smoke-info.txt
    fi
    cat /work/smoke-info.txt

    [ "$result" = written ] && [ -s /work/report.txt ]
}

if [ "${1:-}" = "--inside" ]; then
    inside
    exit $?
fi

# ---------------------------------------------------------------------------
# Host side
# ---------------------------------------------------------------------------

[ "$#" -ge 3 ] || die "usage: qemu-smoke.sh bios|uefi IMG OUTDIR [--append ARGS] [--tag T] [--timeout S] [--net dhcp|none] [--put NAME=FILE]... [--smp N] [--ssh-probe] [--ssh-test-key] [--crlf-boot]"
MODE=$1; IMG=$2; OUTDIR=$3; shift 3
APPEND_ARGS=""; TAG=""; TIMEOUT=900; NET=dhcp; SMP=4
PUT_NAMES=""; PUT_FILES=(); SSH_PROBE=0; SSH_TEST_KEY=0; CRLF_BOOT=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --put)
            [ "$#" -ge 2 ] || die "--put needs NAME=FILE"
            put_name=${2%%=*}; put_file=${2#*=}
            [[ $put_name =~ ^[A-Za-z0-9._-]+$ ]] && [ "$put_name" != "$2" ] \
                || die "--put NAME may hold only letters, digits, '.', '_' and '-'"
            [ -f "$put_file" ] || die "--put: no file at $put_file"
            case " $PUT_NAMES " in *" $put_name "*) die "--put: $put_name given twice" ;; esac
            PUT_NAMES="${PUT_NAMES:+$PUT_NAMES }$put_name"; PUT_FILES+=("$put_file")
            shift 2 ;;
        --append) [ "$#" -ge 2 ] || die "--append needs a value"; APPEND_ARGS=$2; shift 2 ;;
        --tag) [ "$#" -ge 2 ] || die "--tag needs a value"; TAG=$2; shift 2 ;;
        --timeout) [ "$#" -ge 2 ] || die "--timeout needs a value"; TIMEOUT=$2; shift 2 ;;
        --net) [ "$#" -ge 2 ] || die "--net needs a value"; NET=$2; shift 2 ;;
        --smp) [ "$#" -ge 2 ] || die "--smp needs a value"; SMP=$2; shift 2 ;;
        --ssh-probe) SSH_PROBE=1; shift ;;
        --ssh-test-key) SSH_TEST_KEY=1; shift ;;
        --crlf-boot) CRLF_BOOT=1; shift ;;
        *) die "unknown option: $1" ;;
    esac
done
case "$MODE" in bios|uefi) ;; *) die "mode must be bios or uefi" ;; esac
[ -s "$IMG" ] || die "no image at $IMG"
[[ $APPEND_ARGS =~ ^[A-Za-z0-9\ ._=,:/@+-]*$ ]] || die "--append holds a character outside [A-Za-z0-9 ._=,:/@+-]"
[[ $TAG =~ ^[A-Za-z0-9._-]*$ ]] || die "--tag may hold only letters, digits, '.', '_' and '-'"
[[ $TIMEOUT =~ ^[0-9]+$ ]] || die "--timeout must be a number of seconds"
case "$NET" in dhcp|none) ;; *) die "--net must be dhcp or none" ;; esac
[[ $SMP =~ ^([1-9]|1[0-6])$ ]] || die "--smp must be 1 to 16"
if [ "$SSH_TEST_KEY" = 1 ]; then
    [ "$SSH_PROBE" = 1 ] || die "--ssh-test-key needs --ssh-probe"
    case " $APPEND_ARGS " in
        *"=@SSHKEY@ "*) ;;
        *) die "--ssh-test-key: put @SSHKEY@ in --append, as in crtbridge.sshkey=@SSHKEY@" ;;
    esac
fi
command -v docker >/dev/null || die "docker not found"

HERE=$(cd "$(dirname "$0")" && pwd)
SMOKE_IMAGE=$(sed -n 's/^SMOKE_IMAGE=//p' "$HERE/build-env" | tr -d '\r')
[[ $SMOKE_IMAGE =~ ^debian@sha256:[0-9a-f]{64}$ ]] || die "SMOKE_IMAGE in build-env must be debian@sha256:<64 hex>"

mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
rm -f "$OUTDIR/report.txt" "$OUTDIR/serial.log" "$OUTDIR/smoke-info.txt" "$OUTDIR/screen.ppm" \
      "$OUTDIR/mdir.txt" "$OUTDIR/fat-squashfs.sha256" "$OUTDIR/user-env.txt"
rm -rf "$OUTDIR/put"
mkdir -p "$OUTDIR/put"
i=0
for put_name in $PUT_NAMES; do
    cp "${PUT_FILES[$i]}" "$OUTDIR/put/$put_name"
    i=$(( i + 1 ))
done
cp "$IMG" "$OUTDIR/stick.img"

# One local image with QEMU, OVMF, mtools, socat and the ssh client, built
# once per pin and package list (both are in its tag).
SMOKE_PACKAGES="qemu-system-x86 ovmf mtools socat openssh-client"
digest=${SMOKE_IMAGE##*:}
pkg_sum=$(printf '%s' "$SMOKE_PACKAGES" | sha256sum | cut -c1-8)
LOCAL_IMAGE="crt-bridge-smoke:${digest:0:12}-$pkg_sum"
if ! docker image inspect "$LOCAL_IMAGE" >/dev/null 2>&1; then
    printf 'FROM %s\nRUN apt-get update && apt-get install -y --no-install-recommends %s && rm -rf /var/lib/apt/lists/*\n' \
        "$SMOKE_IMAGE" "$SMOKE_PACKAGES" | docker build -q -t "$LOCAL_IMAGE" - >/dev/null || die "cannot build $LOCAL_IMAGE"
fi

kvm=()
[ -c /dev/kvm ] && kvm=(--device /dev/kvm)

set +e
docker run --rm "${kvm[@]}" \
    -e MODE="$MODE" -e APPEND_ARGS="$APPEND_ARGS" -e TAG="$TAG" -e TIMEOUT="$TIMEOUT" -e NET="$NET" \
    -e PUT_NAMES="$PUT_NAMES" -e SMP="$SMP" \
    -e SSH_PROBE="$SSH_PROBE" -e SSH_TEST_KEY="$SSH_TEST_KEY" -e CRLF_BOOT="$CRLF_BOOT" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$HERE:/recipe:ro" -v "$OUTDIR:/work" \
    "$LOCAL_IMAGE" bash /recipe/qemu-smoke.sh --inside
rc=$?
set -e
[ "$rc" -eq 0 ] && printf 'qemu-smoke: %s boot wrote its report\n' "$MODE" || printf 'qemu-smoke: %s boot FAILED (see %s)\n' "$MODE" "$OUTDIR" >&2
exit "$rc"
