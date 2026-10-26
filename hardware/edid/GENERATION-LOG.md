# JVC EDID generation log

Each regeneration appends one row below.

> **Note of 2026-10-16.** No regeneration took place, so no row is added to the table. The EDID in
> service (`jvc_dtv1710cg.bin`) is the product of the `generic_15` fallback of switchres, not of
> the old `jvc_dtv1710cg.ini` file; the details are under "Notes" below. The "Hardware-safety
> assertion" row of generation #1 checked the detailed timing descriptor (DTD) of the binary
> produced, not the old file. Two checksums to keep in mind:
>
> | Binary | sha256 |
> |---------|--------|
> | EDID in service, pinned in the image recipe (`build-image.sh`, `EDID_SHA256`) | `f97d52f5edd4466e0dc4d4a9b2d9a4cfd6eb2681b38d41afd377fb1862986fd6` |
> | what a regeneration from `hardware/monitors/jvc_dtv1710cg.ini` (low band) would give | `e80159d088ddc0e34b8d01ab8b868fcb857571731bff485b2c1ffd0ed2ac79c3` |
>
> Regenerating would break the checksum check of the image: the generation script of the
> development tree (not published) stops with code 4 before any call to switchres.

## Generation #1 -- 2026-04-19

| Field | Value |
|-------|-------|
| Timestamp (UTC) | 2026-04-19T13:22:00Z |
| Host | the build machine (Ubuntu 24.04 LTS, Linux 6.8.0-107-generic) |
| switchres version | 2.2.2 (built from source: `git clone --depth 1 --branch v2.2.2 https://github.com/antonioginer/switchres.git`) |
| Reference mode | 640x240@60 |
| switchres command | `switchres 640 240 60 --monitor custom --ini jvc_dtv1710cg.ini --calc --edid` |
| Computed modeline | `"640x240_60 15.660000KHz 60.000000Hz" 13.013460 640 666 727 831 240 242 245 261 -hsync -vsync` |
| EDID output file | `= custom.bin` (note: switchres named it with the monitor alias from the ini `[switchres]` section; copied to `jvc_dtv1710cg.bin`) |
| Binary size | 128 bytes |
| sha256 | `f97d52f5edd4466e0dc4d4a9b2d9a4cfd6eb2681b38d41afd377fb1862986fd6` |
| DTD H active | 640px |
| DTD H blank | 191px |
| DTD H total | 831px |
| DTD V active | 240px |
| DTD V blank | 21px |
| DTD V total | 261px |
| Pixel clock (quantised) | 13.010 MHz (1301 x 10 kHz, EDID 1.3 granularity) |
| Pixel clock (computed) | 13.013 MHz (switchres internal; difference is EDID quantisation) |
| **H frequency (from DTD)** | **15.656 kHz** |
| **V frequency (from DTD)** | **59.984 Hz** |
| Hardware-safety assertion | **PASSED**: H in [15.625, 16.0], V in [50, 65] |

## Notes

### What switchres actually read (established on 2026-10-16)

The old `jvc_dtv1710cg.ini` never described the tube. Replayed with switchres 2.2.2 built from
the tag (command of the table above, the file byte for byte the one from before), it gives:

- `Invalid option [switchres]`: switchres has no notion of section;
- `Monitor type unknown: = custom`: switchres splits a `key value` line on the space, so the
  value of `monitor = custom` is `= custom`; the type is unknown and switchres falls back,
  silently, to the `generic_15` preset;
- the `crt_range0 = ...` line is never read for the same reason, and it had only 11 fields
  instead of the 16 of the grammar;
- the binary produced is named `= custom.bin` and has the checksum
  `f97d52f5edd4466e0dc4d4a9b2d9a4cfd6eb2681b38d41afd377fb1862986fd6`, that of the EDID in
  service: V range 49-65 Hz, H 15-15 kHz, descriptor name `= custom`, that is the values of
  `generic_15` and not those of the file (50-65 Hz, 15625-16200 Hz).

So this is not a cosmetic defect: the range of the old file was never recognised. The computed
modeline (`13.013460 640 666 727 831 240 242 245 261`) is indeed the one of the table, but it
came out of `generic_15`. The exit code of switchres is 0 in all these cases; a check of a
monitor file with switchres must therefore judge its output, never its exit code.

### Physical EDID of the tube

The JVC DT-V1710CG may send a physical EDID. This EDID was generated to OVERRIDE whatever the
JVC physically sends. The `drm.edid_firmware=` kernel parameter takes precedence over any
physical EDID from the monitor. Whether the JVC sends an EDID or not, this override is applied
at early KMS time.

| Rows added | Automation stage |
|------------|-----------------|
| 1 | initial generation |
