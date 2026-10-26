#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 30-crt-bridge-rcv.sh -- checks what build-image.sh put into the overlay,
# and enables the units. It COMPILES NOTHING.
#
# Why it compiles nothing: switchres is not an Arch package, the daemon is
# linked STATICALLY against libswitchres.a in the build container, and the
# image carries neither gcc nor make.
#
# How it is called: by the "Build hooks" step of customize_airootfs.sh, in a
# child process (`bash "$hook"`, NOT sourced), so that the `set -eu` below
# cannot kill the calling script.
#
# What its exit status does: mkarchiso runs with `set -e -u` and calls
# customize_airootfs.sh as a bare command, so an `exit 1` here stops the build
# BEFORE anything is published. An image without its daemon must never be
# published silently: that is why no check in this file ends in `|| true`
# unless the line says why.
set -eu

BIN_RCV=/usr/local/bin/crt-bridge-rcv
BIN_VOL=/usr/local/bin/hda-volume
BIN_CFG=/usr/local/bin/crt-bridge-config
BIN_REPORT=/usr/local/lib/crt-bridge/write-report
BIN_STICK=/usr/local/lib/crt-bridge/read-stick
BIN_DEV=/usr/local/lib/crt-bridge/dev-mode
UNIT_DEV=/etc/systemd/system/crt-bridge-dev.service
UNITS="/etc/systemd/system/crt-bridge-rcv.service
/etc/systemd/system/crt-bridge-volume.service
/etc/systemd/system/crt-bridge-wol.service
/etc/systemd/system/crt-bridge-report.service
/etc/systemd/system/crt-bridge-config.service
$UNIT_DEV"
ENV_RCV=/etc/crt-bridge/rcv.env
# The monitor file built into the image: the developer mode uses it. The
# public default passes no monitor file at all (the built-in 15 kHz band).
MONITOR_INI=/usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini
LINK_WOL=/etc/systemd/network/10-eth0-wol.link

missing=0

for f in "$BIN_RCV" "$BIN_VOL" "$BIN_CFG" "$BIN_REPORT" "$BIN_STICK" "$BIN_DEV"; do
    if [ ! -x "$f" ]; then
        echo "!!! $f MISSING OR NOT EXECUTABLE -- DO NOT PUBLISH !!!" >&2
        missing=1
    fi
done

for f in $UNITS "$ENV_RCV" "$MONITOR_INI" "$LINK_WOL"; do
    if [ ! -f "$f" ]; then
        echo "!!! $f MISSING -- DO NOT PUBLISH !!!" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "[rcv] contents of /usr/local/bin:" >&2
    ls -la /usr/local/bin >&2 || true
    echo "[rcv] contents of /etc/systemd/system:" >&2
    ls -la /etc/systemd/system >&2 || true
    exit 1
fi

# The public environment passes no monitor file: without a monitor.ini on the
# stick, the daemon must use its built-in 15 kHz band.
if grep -q -- '--monitor' "$ENV_RCV"; then
    echo "!!! $ENV_RCV passes --monitor: the default must be the built-in band -- DO NOT PUBLISH !!!" >&2
    exit 1
fi

# The developer mode opens ssh: its unit must run only when the kernel
# command line says crtbridge.dev=1, and no unit of ours may pull sshd in.
# A Wants= on sshd.service would start it on every boot, condition or not.
if ! grep -qx 'ConditionKernelCommandLine=crtbridge.dev=1' "$UNIT_DEV"; then
    echo "!!! $UNIT_DEV lacks ConditionKernelCommandLine=crtbridge.dev=1 -- DO NOT PUBLISH !!!" >&2
    exit 1
fi
for u in $UNITS; do
    if grep -Eq '^(Wants|Requires|Requisite|BindsTo|Upholds|PartOf|OnSuccess|OnFailure)=.*sshd' "$u"; then
        echo "!!! $u pulls sshd in -- DO NOT PUBLISH !!!" >&2
        exit 1
    fi
done

echo "[rcv] daemon            : $(sha256sum "$BIN_RCV")"
echo "[rcv] hda-volume        : $(sha256sum "$BIN_VOL")"
echo "[rcv] crt-bridge-config : $(sha256sum "$BIN_CFG")"
echo "[rcv] monitor file      : $(sha256sum "$MONITOR_INI")"

# The image is not published if the daemon it carries refuses the monitor
# file it carries. --check-monitor is a dry run: it loads the file, says which
# bands it takes from it, and touches neither DRM nor the network. A non-zero
# status stops the build before anything is published (see above).
echo "[rcv] dry run of the built-in monitor file by the image's daemon:"
if "$BIN_RCV" --check-monitor "$MONITOR_INI"; then
    echo "[rcv] --check-monitor: exit 0"
else
    code=$?
    echo "!!! $BIN_RCV refuses $MONITOR_INI (exit $code) -- DO NOT PUBLISH !!!" >&2
    exit 1
fi

echo "[rcv] dynamic dependencies of the daemon:"
# || true: ldd also exits non-zero on a fully static binary ("not a dynamic
# executable"), not only on a missing dependency. Without it, this bare line
# under set -eu would stop the hook here, before the messages that explain
# the cause. The checks below decide.
ldd "$BIN_RCV" || true
if ldd "$BIN_RCV" | grep -q 'not found'; then
    echo "!!! UNRESOLVED dynamic dependency -- DO NOT PUBLISH !!!" >&2
    exit 1
fi
# libswitchres in the list would mean the build took the dynamic path of the
# Makefile instead of the static archive: the daemon would not start on a
# receiver, which does not carry that library.
if ldd "$BIN_RCV" | grep -q libswitchres; then
    echo "!!! DYNAMIC link to switchres -- STATIC expected !!!" >&2
    exit 1
fi
if ldd "$BIN_CFG" | grep -q 'not found'; then
    echo "!!! UNRESOLVED dynamic dependency of crt-bridge-config -- DO NOT PUBLISH !!!" >&2
    exit 1
fi

install -d -m 0750 /var/log/crt-bridge

# Every unit is enabled in the image itself: a stick boots straight into a
# working receiver.
systemctl enable crt-bridge-config.service
systemctl enable crt-bridge-rcv.service
systemctl enable crt-bridge-volume.service
systemctl enable crt-bridge-wol.service
# The boot report (report.txt on the stick, or the journal on a network boot).
systemctl enable crt-bridge-report.service
# The developer mode: enabled, but skipped by its condition on every boot
# whose kernel command line lacks crtbridge.dev=1.
systemctl enable crt-bridge-dev.service

for u in crt-bridge-config crt-bridge-rcv crt-bridge-volume crt-bridge-wol crt-bridge-report crt-bridge-dev; do
    state=$(systemctl is-enabled "$u.service" 2>/dev/null || echo UNKNOWN)
    echo "[rcv] $u.service is-enabled=$state"
    if [ "$state" != "enabled" ]; then
        echo "!!! $u.service IS NOT ENABLED (state=$state) -- DO NOT PUBLISH !!!" >&2
        exit 1
    fi
done

# sshd itself is never enabled: only the developer mode starts it.
sshd_state=$(systemctl is-enabled sshd.service 2>/dev/null || true)
echo "[rcv] sshd.service is-enabled=$sshd_state"
case "$sshd_state" in
    enabled|enabled-runtime)
        echo "!!! sshd.service is $sshd_state: ssh must open only in developer mode -- DO NOT PUBLISH !!!" >&2
        exit 1 ;;
esac

echo "[rcv] OK: three binaries, write-report, read-stick, dev-mode, six units, rcv.env,"
echo "[rcv] OK: the built-in monitor file and 10-eth0-wol.link present; the six units are enabled;"
echo "[rcv] OK: the developer mode carries its condition, sshd is not enabled."
