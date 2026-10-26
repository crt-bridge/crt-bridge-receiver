#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck disable=SC2034

iso_name="crt-bridge-receiver"
iso_label="CRTBRIDGE_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="crt-bridge <https://github.com/crt-bridge>"
iso_application="crt-bridge receiver image"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y.%m.%d)"
install_dir="arch"
buildmodes=('netboot')
arch="x86_64"
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'xz' '-Xbcj' 'x86' '-b' '1M' '-Xdict-size' '1M')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/root"]="0:0:750"
  ["/root/customize_airootfs.sh"]="0:0:755"
  ["/etc/systemd/system/sshd.service.d/override.conf"]="0:0:644"
  ["/etc/systemd/system-generators/systemd-ssh-generator"]="0:0:644"
  ["/etc/modprobe.d/amdgpu-blacklist.conf"]="0:0:644"
  ["/usr/lib/firmware/edid/jvc_dtv1710cg.bin"]="0:0:644"
  ["/usr/local/bin/crt-bridge-rcv"]="0:0:755"
  ["/usr/local/bin/hda-volume"]="0:0:755"
  ["/usr/local/bin/crt-bridge-config"]="0:0:755"
  ["/etc/systemd/system/crt-bridge-config.service"]="0:0:644"
  ["/etc/systemd/system/crt-bridge-rcv.service"]="0:0:644"
  ["/etc/systemd/system/crt-bridge-volume.service"]="0:0:644"
  ["/etc/systemd/system/crt-bridge-wol.service"]="0:0:644"
  ["/etc/systemd/system/crt-bridge-report.service"]="0:0:644"
  ["/usr/local/lib/crt-bridge/write-report"]="0:0:755"
  ["/usr/local/lib/crt-bridge/read-stick"]="0:0:755"
  ["/usr/local/lib/crt-bridge/dev-mode"]="0:0:755"
  ["/etc/systemd/system/crt-bridge-dev.service"]="0:0:644"
  ["/etc/ssh/sshd_config.d/10-crt-bridge.conf"]="0:0:644"
  ["/etc/crt-bridge/rcv.env"]="0:0:644"
  ["/usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini"]="0:0:644"
  ["/etc/systemd/network/10-eth0-wol.link"]="0:0:644"
  ["/root/customize_airootfs.d/30-crt-bridge-rcv.sh"]="0:0:755"
)
