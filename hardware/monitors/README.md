# Describing a CRT: `hardware/monitors/`

One file per tube (`jvc_dtv1710cg.ini` for the JVC DT-V1710CG). It is the **single source** of the
tube's frequency bands: switchres reads it to compute modelines, and the receiver's guard
(`crt-bridge-rcv --monitor PATH`) reads the same text to refuse any mode the tube cannot take.

## The limit, before any example

**The file can only narrow the bands inside a fixed envelope compiled into the daemon's `safety.h`.**
The envelope never depends on the file (if this table and `safety.h` disagree, `safety.h` wins):

| Quantity | Fixed envelope |
|---|---|
| Horizontal (H), low band | 15 625 to 16 000 Hz |
| Horizontal (H), high band | 27 000 to 45 000 Hz |
| Dead band (16 001 to 26 999 Hz) | always refused |
| Vertical (V) | 50 to 80 Hz |

- **"Without touching code" only holds for a tube whose bands fit inside this envelope.**
- **A tube with a band outside it (a 24 kHz monitor, a 15 kHz tube below 15 625 Hz) requires
  editing `safety.h`.** A file cannot widen the envelope (reason `ENVELOPE`) or touch the dead band
  (`DEAD_BAND`). This is deliberate: the envelope is the last protection of the hardware and must
  not depend on a text anyone can edit. Changing it is a code change, with tests and review.
- **Two more conditions:** the file needs a low band (H max at most 16 000 Hz) that contains the two
  modes pinned in the daemon's `modelines.h` (15 699.48 Hz line rate; 59.92 Hz in 240p, 59.81 Hz per
  field in 480i). A low band that excludes them loads, then the daemon refuses to start.

## The file format

- one `monitor custom` line, then `crt_range0` to `crt_range9` lines (each index once);
- comments start with `#` at the **start of a line**; no `=` sign, no trailing comment, no
  indented line;
- **LF line endings only**, UTF-8 **without BOM**, 4 096 bytes per file, 255 per line.

A `crt_range` line has **sixteen fields** (switchres 2.2.2). The first four are hyphen-joined pairs
with no spaces (`Hmin-Hmax`, `Vmin-Vmax`); the rest are comma-separated plain decimals.

| Fields | Content | Unit |
|---|---|---|
| 1, 2 | H min, H max | Hz |
| 3, 4 | V min, V max (**field** rate when interlaced) | Hz |
| 5, 6, 7 | H porches: front, sync, back | **us** (microseconds) |
| 8, 9, 10 | V porches: front, sync, back | **ms** |
| 11, 12 | H sync polarity, V sync polarity (`0` negative, `1` positive) | |
| 13, 14 | progressive lines: min, max (`0`, or at least 128 for the min) | lines |
| 15, 16 | interlaced lines: min, max (`0, 0` = none in this band) | lines |

**H porches are in microseconds, V porches in milliseconds.** Writing microseconds where
milliseconds are expected makes switchres silently
drop the band while still exiting 0; the receiver applies the same rules and refuses it
(`SWITCHRES_RULE`). The JVC low band:

```text
crt_range0 15625-16000, 50-80, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576
```

### Status comments

Above **every** `crt_range` line, a comment says where the values come from. The guard ignores it.
Write one of these two strings verbatim:

- `# documented (manual), not verified`: values come from the tube's documentation; no session at
  the tube has confirmed them. Every band starts here.
- `# verified on this tube only on YYYY-MM-DD`: the tube this file describes was seen holding this
  mode. Switch **only** after a session at that tube, and write after the date what was seen (a
  point, or the whole band). It says nothing about any other tube, even of the same model.

In the JVC file both bands are verified on that one tube, at one point each: the low band at
15,699.5 Hz (240p and 480i) on 2026-10-16, the high band at 31,468.75 Hz (480p) on 2026-10-18.
Neither band is widened without a new session at the tube.

## The JVC high band: open to the 480p mode alone

The high band (31 kHz) is open to **one mode**: the 480p regime, a fixed 1280x480 progressive
constant of `modelines.h` (50.35 MHz clock, 1600x525 total, 31 468.75 Hz line rate), judged
under the high band alone. Any other request that lands in the high band is served by that same
constant, never by the timing on the wire. The 240p and 480i modes are judged under the low band
alone.

- **Default path:** the receiver gives switchres the low band, plus the high band if and only if the
  480p mode is enabled. A 480p refused by the guard at boot, or by switchres at registration, is
  disabled; the daemon still starts. The journal says which: `regime_enabled` or `regime_disabled`.
- **A 480p request to a receiver whose 480p is disabled is served as 480i, not refused.** This
  covers a tube with no high band, a guard or switchres that refused the 480p, and a receiver that
  has not judged it yet. The whole 640x480 image is scanned in two fields, in the low band: no band
  is crossed and the screen does not go black. The wire format stays the one of the 480p (whole
  frames), so the sender needs no change. The journal writes one `regime_fallback` line each time a
  receiver enters this fallback, with its reason (`no_high_band`, `guard_refused`,
  `switchres_refused` or `not_ready`). The fallback does not keep the pace of a true 480i: the sender
  does not know that the receiver fell back, so it still paces for 59.94 Hz and the 480i scans at
  59.81 Hz. It is a safety net, not a way to play.
- **A receiver in 480p returns the tube to 15 kHz when the frames stop.** After `--idle-after`
  seconds without frames (10 by default) it leaves the high band through the normal band crossing
  (journal `band_crossing_begin` with reason `session_end`) and draws its waiting screen in 15 kHz.
  When frames come back without a new announce, it republishes the announced mode (journal
  `regime_resume`). A mode change at the tube with no request sent is this return, not a fault.
- **`--legacy-modeset` path (the escape hatch, never in service):** low band alone, switchres
  included, and no 480p. The receiver does not judge the 480p on this path: the journal says
  `regime_disabled` with reason `legacy_modeset`, and never `regime_enabled`. Reason for the low
  band alone: switchres sets the first mode before the guard judges it. Knowing the 31 kHz band, it
  would pick it for a 480-line request and drive it onto the tube before it was refused. With the
  low band alone, a 640x480 at 60 Hz request stays in the 15 kHz band: on the JVC, switchres set
  640x240 progressive at 15.655 kHz (seen at the tube on 2026-10-16).
- The bands given to switchres are renumbered from `crt_range0`. The log line
  `switchres_configured` gives the `described` and `given` band counts.

## What the file does not decide

- **The geometry actually scanned in 240p, 480i and 480p is the one in `modelines.h`**
  (240p and 480i: 2560 active pixels, 48.48 MHz clock, 3088 pixels per line; 480p: 1280 active
  pixels, 50.35 MHz clock, 1600 pixels per line). The file's porches only feed switchres'
  modelines; changing one does not change what the tube receives.
- **On the receiver's stock kernel switchres creates no mode at all.** Its drmkms backend cannot
  add user modes, so `sr_add_mode` answers, whatever is asked, with the one mode the connector
  already has (the EDID's 640x240 at 15.655 kHz; replayed off-bench against switchres 2.2.2). For
  the 480p the daemon journals `switchres_returned_existing_mode` and does not judge that answer;
  the guard's verdict stands. When no mode of the connector lies in the given bands (a tube on
  another output, with no 15 kHz mode in its list), `sr_add_mode` refuses: the daemon journals
  `switchres_no_existing_mode` and goes on, never stopping on it, since the guard alone judges the
  modes sent to the card.
- **EDID is out of scope.** The file does not affect the receiver's EDID (a single 15 kHz band).
  No band is ever chosen by swapping EDIDs; the guard applies them (see `hardware/edid/README.md`).

## Describing another tube

1. **Check the envelope** table above. If a band falls outside, stop: edit `safety.h` first.
2. **Copy the JVC file**, rename it, adjust the header comment, bands and porches. Each
   `crt_range` gets the `# documented (manual), not verified` comment: the JVC file's
   "verified" lines are true of that one tube only.
3. **Dry run** with the daemon binary (Linux, built from the daemon sources):

   ```text
   crt-bridge-rcv --check-monitor hardware/monitors/jvc_dtv1710cg.ini
   ```

   It uses the startup loader and lights up nothing (no display, network or log). Exit **0**: the
   daemon would start with this file. Exit **3**: it would refuse it, either because the loader
   rejects the file or because the low band excludes a pinned mode (`verdict=BAND`). Output for the
   JVC file:

   ```text
   monitor: hardware/monitors/jvc_dtv1710cg.ini bytes=589 bands=2
   band 0: key=crt_range0 line=6 h=15625.00-16000.00 v=50.00-80.00 usable=yes usable_480p=no
   band 1: key=crt_range1 line=8 h=31000.00-32000.00 v=59.00-61.00 usable=no usable_480p=yes
   source 240p: h_hz=15699.48 v_hz=59.92 verdict=ACCEPT band=0 mode=2560x240 regime=240p_super_res
   source 480i: h_hz=15699.48 v_hz=59.81 verdict=ACCEPT band=0 mode=2560x480i regime=480i
   source 288p: h_hz=15625.00 v_hz=50.08 verdict=ACCEPT band=0 mode=none (no pinned mode: band only)
   source 480p: h_hz=31468.75 v_hz=59.94 verdict=ACCEPT band=1 mode=1280x480 regime=480p
   ```

   Each band has two flags, one per mask: `usable=` is the low-band mask (the 240p and 480i
   modes), `usable_480p=` the high-band mask (the 480p mode only). No mask holds both bands, so
   the high band is open to the 480p mode alone. `source 240p`, `480i` and `480p` are the three
   pinned modes: the 480p line judges the fixed 1280x480 constant of `modelines.h`, under
   the high band only. A tube with no high band that hosts it keeps its 480p disabled: the line
   ends in `mode=none (regime 480p disabled)`, and the daemon still starts. The `288p` line is a
   reference timing with no pinned mode (`mode=none`), so only the band verdict counts. The exit
   code counts only the 240p and 480i modes: a refused 480p never turns it to 3. `verdict=` is
   `ACCEPT` or a reason (`H_LOW`, `H_DEAD_BAND`, `H_HIGH`, `V_OUT_OF_RANGE`, `BAND`); `host=` names
   the band that would take the timing if all were open. On the 480p line, `verdict=ACCEPT` says
   what the guard judges; whether the 480p is served is said by the startup journal
   (`regime_enabled` or `regime_disabled`), because switchres has a say too on the real display.
   A refused file gives one line with the loader's reason and the faulty line, for example
   `monitor: tube.ini REFUSED reason=DEAD_BAND (band touches the 16-27 kHz dead band) line=6`. Other
   common reasons: `UNKNOWN_KEY`, `FIELD_COUNT`, `FIELD_SYNTAX`, `TRAILING`, `CRLF`, `OVERLAP`,
   `NO_LOW_BAND`.

4. **Have switchres read it too** (optional; the development tree's tool for this is not
   published). Run switchres 2.2.2 on the file and judge **its output**, never its exit code:
   switchres fails open (exit 0 even after dropping a band). It must return as many ranges as the
   file describes.
5. **A refused file at boot:** the daemon logs `monitor_rejected` (reason and line) and exits with
   **code 3**. systemd does not restart it, since retrying cannot fix a bad file. Fix it and go
   back to step 3.

## In the image

The receiver lives in RAM, so nothing written on it survives a reboot.

- **By default no file is given:** the image's `rcv.env` passes no `--monitor`, and the daemon
  takes its built-in 15 kHz band (the JVC's `crt_range0`, byte for byte).
- **Another tube:** put its file, named exactly `monitor.ini`, at the root of the boot stick. At
  every boot it is checked and copied to `/run/crt-bridge/monitor.ini`, and the daemon is started
  with `--monitor` on that copy. A refused file stops the daemon (exit code 3); it never falls back
  to the built-in band. The stick carries the JVC file as `examples/monitor-jvc_dtv1710cg.ini`.
- **The JVC file is also built into the image**, by `build-image.sh`, as
  `/usr/share/crt-bridge/monitors/jvc_dtv1710cg.ini`. Only the developer mode (`crtbridge.dev=1`
  on the kernel command line) uses it; the public default does not.
- **The image hook refuses what the daemon would refuse:** it runs `--check-monitor` on the built-in
  file, and on exit code 3 the build stops.
- **To see what is running:** the boot report on the stick (`check_monitor` lines), the
  `monitor_loaded` journal line, or `crt-bridge-rcv --check-monitor /run/crt-bridge/monitor.ini` on
  the receiver (safe while the daemon runs).
