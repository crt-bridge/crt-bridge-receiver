crt-bridge receiver - boot stick
================================

This stick starts a PC as a crt-bridge receiver: it shows on a CRT the
picture an emulator sends over the network. Boot the PC from the stick (the
boot menu of its BIOS); nothing is installed on the PC's own disks.

What the receiver needs
-----------------------

- 4 logical CPUs at least (2 cores with Hyper-Threading, or 4 cores).
  With fewer the receiver still starts, and report.txt warns.
- A wired network connection. An address is taken by DHCP; without a DHCP
  server the receiver keeps a 169.254.x.x address.
- BIOS boot is the tested way. UEFI boot is provided but NOT VERIFIED ON
  HARDWARE (only in a virtual machine). For UEFI, Secure Boot must be
  disabled in the firmware settings.

The tube: monitor.ini
---------------------

By default the receiver uses only its built-in 15 kHz band, the one every
standard-resolution CRT takes. Nothing to do for such a tube.

To describe another tube, put a file named monitor.ini at the root of this
stick. Its format is the "crt_range" format of switchres; examples/ holds
one. A monitor.ini can only narrow the bands inside the limits built into
the receiver, never widen them.

examples/monitor-jvc_dtv1710cg.ini is the file of one tube, a JVC DT-V1710CG.
Besides the 15 kHz band it carries a 31 kHz band, verified on that tube only:
do not use it for another tube.

If monitor.ini is refused (a wrong value, or a file saved as "Unicode"
instead of plain text), the receiver does NOT fall back to the built-in
band: it stops, and report.txt says why. Files written by Windows Notepad
(CRLF line endings, a UTF-8 BOM) are accepted. Windows may hide the ".txt"
of "monitor.ini.txt": the file must be named exactly monitor.ini.

The settings: settings.ini
--------------------------

settings.ini, at the root of this stick, holds the other settings (volume,
waiting screen, sound, inputs, extra options). Each one is explained in the
file; remove the "#" in front of a line to use it.

The report: report.txt
----------------------

At each start the receiver writes report.txt at the root of this stick,
once, about 20 seconds after the receiver program has started. It tells the
version, the kernel, the number of CPUs, the network cards, the addresses,
what was read from monitor.ini and settings.ini, and why the receiver holds
or stopped.

Cutting the power while report.txt is being written may leave the stick
needing a repair under Windows (Windows offers it; the stick still works).
Wait about a minute after power-on before switching off.

report.txt holds the receiver's local network address and the hardware
(MAC) address of its network card. It holds no password and no key. Look
it over before you share it, for example to ask for help.

Writing the stick again
-----------------------

Writing the stick image again (Rufus, balenaEtcher, dd) erases the whole
stick, monitor.ini and settings.ini included. Copy them elsewhere first.
