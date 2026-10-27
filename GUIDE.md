# From an empty PC to a picture on the tube

This guide puts the published pieces of crt-bridge in order, from a blank receiver PC to a game
on a CRT. It assumes you know your way around a BIOS, a local network and RetroArch. Each piece's
own README has the details; this page only links to them.

| Piece | Release | What it does |
|---|---|---|
| Receiver | https://github.com/crt-bridge/crt-bridge-receiver/releases/tag/v0.1.0 | A USB stick image. The PC it boots drives the CRT. |
| Emitter | https://github.com/crt-bridge/RetroArch/releases/tag/v1.22.2-crtbridge.4 | A RetroArch build for Windows. It runs the game and sends every frame to the receiver. |
| Client (optional) | https://github.com/crt-bridge/libretro-crt-bridge/releases/tag/v0.1.0 | A libretro core that shows the same stream on a second computer. |

All three are pre-releases.

## The tube comes first

- The receiver drives only its built-in 15 kHz band, unless a `monitor.ini` on the stick
  describes your tube. See "The tube comes first" in [README.md](README.md).
- The receiver PC's BIOS shows its own screens at 31 kHz for a few seconds at every boot, before
  any crt-bridge software runs. Judge whether your tube tolerates that before you connect it.
- Connect the tube to the DVI-I output of the graphics card. Only that connector gets the forced
  15 kHz EDID.
- The emitter protects no tube. Leave `Settings > Recording > Receiver 31 kHz` Off, the default,
  unless your tube takes 31 kHz. The emitter's own README, inside its archive, opens with the
  same warnings ("Before you connect a CRT").
- Do not turn on the receiver's developer mode with your tube: it drives a 31 kHz band whatever
  tube is connected.

## 1. The receiver is alive

1. From the receiver release, download `crt-bridge-receiver-v0.1.0.img` and `SHA256SUMS`, then
   check the image: `sha256sum -c --ignore-missing SHA256SUMS` on Linux, or
   `Get-FileHash -Algorithm SHA256 crt-bridge-receiver-v0.1.0.img` in PowerShell, compared with
   the first line of `SHA256SUMS`.
2. Write the image to a USB stick with Rufus, balenaEtcher or `dd`. This erases the whole stick.
3. Wire the receiver PC to your network (the address comes from DHCP), connect the tube, and boot
   the PC from the stick through its BIOS boot menu. BIOS mode is the verified way; for UEFI,
   Secure Boot must be disabled. The PC needs 4 logical CPUs: see "What the PC needs" in
   [README.md](README.md).
4. The tube shows the waiting screen: `CRT-BRIDGE`, `WAITING`, the receiver's address, and the
   time. Write the address down: the emitter needs it.

If the text console keeps the screen instead, ending with
`crt-bridge receiver - <address> - see report.txt on the stick`, the receiver has stopped. The
reason is in `report.txt` at the root of the stick. It is written about 20 seconds after start:
wait about a minute after power-on before you switch off and read it.

## 2. The full chain

1. On a Windows PC on the same network, download
   `crt-bridge-emitter-v1.22.2-crtbridge.4-windows-x64.zip`, compare its SHA-256 with the line in
   the release notes, and unpack it.
2. Put your own libretro cores in its `cores/` folder. None is included.
3. Start `start-emitter.cmd`. The binaries are not code-signed: SmartScreen or Smart App Control
   may step in, and the archive's `README.md` ("Windows notes") says what to do.
4. Open the menu (F1 on a keyboard, or hold Down + Y + L + R on a gamepad), go to
   `Settings > Recording > Groovy Master Address`, and type the address read on the waiting
   screen. The port is 32100 when omitted. There is no default address: until one is set,
   nothing is sent.
5. Load a core, then a game (`Load Core`, then `Load Content`). The tube gets a picture only once
   a game runs. If you set the address after loading the game, close it (`Close Content`) and
   load it again.

The receiver on the tube is the master: it must be wired. A gamepad plugged into the receiver
drives the game, and the sound plays from the receiver PC's sound output. To show the stream on a
second computer as well, follow the client's
[INSTALL.md](https://github.com/crt-bridge/libretro-crt-bridge/blob/main/INSTALL.md); over Wi-Fi,
turn `Settings > Recording > Padded Session` On in the emitter (never towards a MiSTer). The
protocol has no authentication and no encryption: keep it on your local network.

## 3. The right picture

- The emitter sends every frame at the core's native resolution, and the receiver sets a matching
  video mode for it on the tube.
- A 240-line game is shown in 240p, progressive. A picture taller than 280 lines is sent as
  interlaced 480i, or as 480p when Receiver 31 kHz is On (31 kHz tubes only). When a game switches
  between the two, the tube follows.
- PAL (576 lines) is refused: nothing is sent, and the PC screen says so.
- A core that renders above its native size is reduced before sending. A line on the PC screen
  says what was done, for example
  `Supersampling reduction: Auto -> 2x (1280x864 -> 640x432), GPU, vulkan`. The size on the
  right is what the tube gets.

## When something is wrong

| Symptom | Cause | Fix |
|---|---|---|
| The waiting screen stays, no picture | No game is running yet, no address is set, or the address was set after the game was loaded | Load a game. Set `Groovy Master Address`. Close the content and load it again. |
| The waiting screen stays, though a game runs and an address is set | Wrong target address: the receiver's DHCP address changed, or an old one is still in the menu. The picture may be going to another machine. | Compare `Groovy Master Address` with the address on the waiting screen. A DHCP reservation for the receiver keeps it stable. |
| Windows asks whether to allow network access | A program started for the first time from a new folder; the client also listens on UDP 32100 and 32101 | Allow Private networks only, and check that your home network is classified Private. |
| The picture is flattened, half the height of the tube, very low resolution | `Supersampling reduction` forced to a larger factor than the core renders at (8x on a 4x render: 160x120 instead of 320x240) | Set it back to Auto, or to the core's own factor. It takes effect when the content is next loaded; check the line on the PC screen. |
| Picture, but no sound | `audio = off` or a low `volume` in `settings.ini` on the stick; `Settings > Recording > Groovy Audio` set to off on the emitter; the tube's audio input not connected to the receiver PC | Fix the setting (`Groovy Audio` = receiver, the default), or the cable. |
| A text console above 15 kHz at boot: unstable picture, or a whistling tube | The tube is on another connector than DVI-I, so the forced 15 kHz EDID does not apply | Switch the tube off and move it to the DVI-I output. |
| The console keeps the screen, no waiting screen | The receiver stopped, for example on a refused `monitor.ini` | Read `report.txt` at the root of the stick. |
