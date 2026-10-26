# JVC DT-V1710CG custom EDID

Authoritative binary: `jvc_dtv1710cg.bin` (128-byte EDID 1.3 base block).
Origin: the `generic_15` fallback of switchres 2.2.2 (V range 49-65 Hz, H 15-15 kHz, descriptor
name "= custom"). The `jvc_dtv1710cg.ini` file of this folder was meant to describe the tube, but
switchres never read it; the pinned switchres tool replays the command of the generation log and
rebuilds this binary byte for byte. sha256 checksum:
`f97d52f5edd4466e0dc4d4a9b2d9a4cfd6eb2681b38d41afd377fb1862986fd6`, pinned in the image recipe
(`build-image.sh`, `EDID_SHA256`). The description of the tube lives in
`hardware/monitors/jvc_dtv1710cg.ini`.

## Why this exists

The JVC DT-V1710CG has a **hardware dead band at 16-26 kHz horizontal**.
Any modeline driving H in that range will either refuse to sync or, if the
driver holds the mode under signal, risk deflection circuit damage.

The off-the-shelf `arcade_15_25_31.bin` EDID (ships with switchres) declares
a "25 kHz" band that lands squarely in the JVC dead band. Loading it via
`drm.edid_firmware=` on a receiver wired to this monitor is a hardware-loss
risk. This repo's EDID replaces it with a single 15 kHz range (switchres' `generic_15`).

## Scope -- one band only, 15 kHz

An EDID 1.3 base block carries only ONE range descriptor. The JVC has two disjoint safe bands
(15 kHz and 31 kHz); the EDID announces only one, with a preferred mode at 15 kHz. Bands are never
chosen by swapping EDIDs: they are described in `hardware/monitors/`, and the receiver's guard
applies them.

The two-band guard that REFUSES dead-band modelines is NOT a property of the EDID: it is a software
check in the receiver daemon (`safety.h`, `safety.c`). Do not confuse the two.

## Do not regenerate

The EDID in service is not regenerated. Regenerating from the corrected file
(`hardware/monitors/jvc_dtv1710cg.ini`) would give another binary, with checksum
`e80159d088ddc0e34b8d01ab8b868fcb857571731bff485b2c1ffd0ed2ac79c3` (V range 50-80 Hz, H 15-16 kHz,
name `custom`), and would break the checksum check of the receiver image. The generation script
of the development tree (not published) stops by itself, with code 4, before any call to
switchres; its body stays as a record of the original recipe.

## Preferred-timing safety check

To read the timing descriptor of the binary in service:

    # Byte 0x36-0x47 is the first detailed timing descriptor
    xxd -s 0x36 -l 18 hardware/edid/jvc_dtv1710cg.bin

Compute H = pixel_clock / H_total where:
  - pixel_clock is bytes 0-1 of the DTD (LE, units of 10 kHz)
  - H_active  = byte 2 + ((byte 4 & 0xF0) << 4)
  - H_blank   = byte 3 + ((byte 4 & 0x0F) << 8)
  - H_total   = H_active + H_blank

REJECT the binary if H is NOT in [15.625, 16.0] kHz.

The automated check is the "Hardware-safety assertion" of that generation script. It aborts
with an error if the binary is hardware-unsafe (the script stops before it today, see above). Do
NOT commit a binary for which this check has not passed.
