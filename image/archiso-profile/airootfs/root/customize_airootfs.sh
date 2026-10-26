#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# customize_airootfs.sh -- runs in arch-chroot AFTER pacstrap, BEFORE the
# squashfs is assembled. archiso marks the mechanism deprecated, but it still
# works, and it is the right place for work that must follow the package
# install: the file overlay is copied before pacstrap, so a file owned by a
# package would be overwritten by it.
#
# The image is public: nothing here opens it to anyone. root stays locked
# (the filesystem package writes `root:*`, and the overlay's /etc/shadow says
# the same), sshd is not enabled, and no console logs in by itself.

# No `set -e`: every step is attempted even when an earlier one fails, and
# the log says what happened. The build hooks below decide the exit code.

LOG=/root/customize.log
echo "=== customize_airootfs.sh start $(date -Iseconds) ===" > "$LOG"

# --- Services --------------------------------------------------------------
{
  systemctl enable systemd-networkd.service && echo "[svc] systemd-networkd enabled"
  systemctl enable systemd-resolved.service && echo "[svc] systemd-resolved enabled"
  systemctl set-default multi-user.target && echo "[svc] default target = multi-user"
} >> "$LOG" 2>&1

# --- SSH host keys, generated at the first start of sshd ---------------------
# sshd is not enabled: this unit only runs if something starts sshd. No host
# key is ever built into the image.
cat > /etc/systemd/system/sshd-keygen.service <<'EOF'
[Unit]
Description=Generate SSH host keys on first boot
ConditionPathExists=!/etc/ssh/ssh_host_ed25519_key
Before=sshd.service

[Service]
Type=oneshot
ExecStart=/usr/bin/ssh-keygen -A

[Install]
WantedBy=sshd.service
EOF
systemctl enable sshd-keygen.service >> "$LOG" 2>&1 && echo "[svc] sshd-keygen enabled" >> "$LOG"

# --- radeon module load on boot ----------------------------------------------
# amdgpu is blacklisted via /etc/modprobe.d/amdgpu-blacklist.conf.
# radeon should load automatically via the kms hook of mkinitcpio; the
# modules-load.d entry makes sure it is loaded in multi-user as well.
cat > /etc/modules-load.d/radeon.conf <<'EOF'
radeon
EOF
echo "[modules] radeon module-load.d entry written" >> "$LOG"

# --- Regional settings, neutral, so that systemd-firstboot asks nothing ------
# Without them, a fresh boot would stop on tty1 prompts (time zone, locale,
# host name). systemd-firstboot skips whatever is already set. The keyboard
# (us) comes from /etc/vconsole.conf in the overlay. C.UTF-8 is built into
# glibc: no locale-gen needed.
ln -sf /usr/share/zoneinfo/UTC /etc/localtime
echo 'LANG=C.UTF-8' > /etc/locale.conf
echo 'crt-bridge' > /etc/hostname
echo "[regional] time zone UTC, locale C.UTF-8, host name crt-bridge" >> "$LOG"

# --- Marker, to confirm this script ran --------------------------------------
echo "customize_airootfs.sh ran at $(date -Iseconds)" > /etc/customize-ran

# --- initramfs, regenerated with our HOOKS -----------------------------------
# The mkinitcpio run of pacman's hook during pacstrap comes BEFORE our
# overlay is fully in place, so the archiso HOOKS of /etc/mkinitcpio.conf are
# not applied. Regenerate now, inside the chroot, when the world is stable.
{
  echo "[mkinitcpio] regenerating initramfs with archiso HOOKS..."
  mkinitcpio -p linux-rt
  echo "[mkinitcpio] done. Listing /boot:"
  ls -la /boot/
} >> "$LOG" 2>&1

# --- Build hooks ---------------------------------------------------------------
# Each hook runs in a child process (`bash`, NOT `source`): a hook's own
# `set -eu` must not be able to stop this script before the marker or the
# initramfs above. build-image.sh copies the hooks into
# airootfs/root/customize_airootfs.d/, so into the chroot.
HOOK_DIR=/root/customize_airootfs.d
HOOK_FAILED=0
if [ -d "$HOOK_DIR" ]; then
  for hook in "$HOOK_DIR"/*.sh; do
    [ -f "$hook" ] || continue
    echo "[hook] === $hook ===" >> "$LOG"
    bash "$hook" >> "$LOG" 2>&1
    rc=$?
    echo "[hook] $hook exit=$rc" >> "$LOG"
    [ "$rc" -eq 0 ] || HOOK_FAILED=1
  done
  # The hooks must not end up in the squashfs.
  rm -rf "$HOOK_DIR"
else
  # A missing directory is a failure, not a normal case: without the hooks,
  # the image would be built without any of their checks having run.
  echo "[hook] NO HOOK: $HOOK_DIR missing, build-image.sh did not copy the hooks" >> "$LOG"
  HOOK_FAILED=1
fi

echo "=== customize_airootfs.sh end $(date -Iseconds) ===" >> "$LOG"

# A failed or missing hook stops mkarchiso, hence the build and any
# publication. mkarchiso runs with `set -e -u` and calls this script as a bare
# command: a non-zero exit code is all it takes.
if [ "$HOOK_FAILED" -ne 0 ]; then
  # The log dies with the chroot: printing it on stderr is the only way to
  # see WHY the build stopped.
  echo "=== HOOKS FAILED, build stopped ===" >&2
  cat "$LOG" >&2
  exit 1
fi

# archiso deletes this script after running it; the log and the marker stay.
exit 0
