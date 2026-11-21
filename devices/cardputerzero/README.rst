Quadband Card Studio for CardputerZero
====================================

Development application 0.3.0 for the Raspberry Pi CM0/aarch64 Linux
CardputerZero and Zero-Lite. This is a separate keyboard-first application,
not ESP32 firmware, an OS image, or a CoreS3 webflasher payload. Existing
CoreS3 and desktop targets share the corresponding guarded NDEF policy.

The application shares the production C++ standalone engines and write
guards with CoreS3. It provides Grove RFID/RFID2 HF memory, all 19 NDEF
record builders and RAM-copy workflows; bounded M100 UHF inventory,
selected memory/User/EPC/power tools; and capability-driven Chameleon Ultra
LF scan/custom-ID/password-free T5577 workflows. Protected UID/trailer,
metadata, lock/password and forbidden UHF operations remain disabled.
Device launch automatically identifies known attached modules, but never
starts a card scan, tag inventory, write or Sub-GHz receive/transmit operation.

Card contents, source captures, draft records and supplied credentials
remain RAM-only. File browsing/import/export, new reader families and a
Linux BLE bridge server are not part of this version. The simulator has
synthetic NTAG/UHF data and read-only EM410x LF; its results are not hardware
evidence.

Connections
-----------

The normal device launch checks only the known onboard endpoints: the
right-hand Grove UART (/dev/serial0), RFID/RFID2 at I2C1 address 0x28, and
the Cap identity registers at /dev/spidev0.1 and /dev/spidev0.2. No broad
serial enumeration or I2C address scan is performed. Supported identified
readers are connected automatically; one absent reader does not hide another.
Use Detected hardware / status for per-module results and prerequisites,
or Connections > Auto-detect / rescan known hardware to repeat the checks.
Manual endpoint fields and Connect remain available; --no-autodetect
disables startup identification.

* HF: stock-OS Linux I2C device, RFID or RFID2 at address 0x28.
  The Cap's separate I2C Grove port can host RFID2 while the Zero's right
  Grove stays in UART mode for UHF. Detection verifies I2C1 pin routing and
  the chip version before selecting it; manual HF configuration requires
  confirmation of that chosen I2C connector. The app never changes the
  Grove mux, resets the shared keyboard/USB hub or broadly probes I2C.
* UHF: an explicitly selected UART device at the configured baud,
  default 115200. Only recognized M100/JRD/YRM-family module identities
  are accepted. The app does not disable the Linux serial console.
* LF: official supported Chameleon Ultra 2.x USB CDC serial endpoint,
  with the physical USB switch in Host mode. Real firmware/model/command
  capabilities are checked; missing capabilities are not fabricated.

The Zero itself has ONE Grove socket, switchable between I2C and UART,
not separate CoreS3 Port A/Port C. With Cap CC1101, its additional I2C
Grove connector provides another HF connection; without a Cap, use an
additional connection such as USB-serial for simultaneous HF/UHF.
Grove output is documented below 500 mA; verify the readers' actual power
budget and use appropriate external power. When the known board and UART
route match, the app enables only the dedicated right-Grove 5V LED-class
output and retains it on exit. A narrow package udev rule gives gpio-group
users write access to only that brightness attribute; no root GUI or sudo
helper is installed. The app does not enable USB/Cap supplies, cut shared
power, or repurpose UART pins.

Cap CC1101 and its ST25R3916 NFC chip are identified separately using
read-only SPI registers when their existing OS power/overlay prerequisites
are met. This is hardware identification only: built-in ST25R3916 card
tools and CC1101 Sub-GHz workflows are not implemented. HF tools still
use the external RFID/RFID2. The stock CC1101 initializer is NOT called,
because it repurposes GPIO14/15 that are needed by the Grove UART.

Run as the ordinary device user with the necessary video/input/dialout/I2C
permissions. No broad sudoers rule or root GUI is installed. Connections
are exclusive where supported; I2C flock is advisory and cannot stop an
uncooperative external process from using the same bus.

Keyboard and safety
-------------------

Use native arrow/Fn navigation or Tab/Shift+Tab to move focus; Enter
activates the focused control. Text fields accept printable ASCII,
Backspace/Delete and Home/End. Esc cancels an editor/review or returns one
level; Esc at Home exits. Ctrl+C stops the reader job, Ctrl+Q exits.

The 0.2.0 interface opens the working screens directly, not an intermediate
tool menu. Press 1 for HF memory, 2 for UHF inventory, or 3 for LF scan,
then R to explicitly Read/Start/Scan. R or Esc stops busy work. Re-selecting
the current reader returns locally without clearing its results or starting
RF. Read/Scan/Stop and both review decisions stay on-screen. After inventory
is stopped, selecting a UHF tag opens its memory workspace directly.

Context shortcuts are shown on the compact action bars:

* HF: E edits bytes, U selects the unit, A opens Classic auth, T opens
  NTAG auth, N opens NDEF, C opens RAM copy and M returns to memory.
* UHF: M opens memory, B bank, G range, D data, E EPC, A access password
  and P RF power from inventory. R reads the current visible primary
  control, including RF power.
* LF: C captures the scanned ID, F chooses format and I opens ID/write
  controls. Capture requires a scanned identity.
* NDEF: K chooses one of all 19 kinds, F edits fields and V views records.
* S opens Settings; H shows identified hardware. About retains project
  credits; theme, connection forms, session clear and Quit are in Settings.

W only prepares an applicable visible write review; it never confirms it.
Reader/action shortcuts are disabled in reviews and busy states except
safe Stop/exit/inspection. Inside an editor they are ordinary text, not
commands. Forms paginate to keep controls compact; PgUp/PgDn inspect
bounded result/review details. Focus follows stable actions/tag identities
rather than changing result strings, and unchanged snapshots do not redraw.

Writes default to Cancel and require a fresh key activation after the
displayed review. Held keys and repeats cannot replay a write. A worker
serializes device access; settings and tag selection are unavailable while
inventory is active. Live inventory still shows EPC/RSSI/read counts with
local pagination. Stop retains results, and interrupted writes remain
visibly unverified.

The launcher keeps a shared keyboard descriptor for long-Esc supervision;
this application does not grab the keyboard. SIGTERM/SIGINT set a shutdown
flag, never perform I/O in the signal handler, and request worker
cancellation/cleanup. The reader adapter shares a two-second cancellation
budget across RF-off, UHF Stop and original Ultra mode restoration/readback.
Unverified release quarantines the connection. This is not a hard real-time
guarantee: Linux scheduling and the I2C driver's compliance with its
configured timeout still require target validation. Forced SIGKILL/power
loss cannot guarantee cleanup.

Bulk NDEF jobs
--------------

In HF, open NDEF and use B for Bulk 1--3 records. Build the RAM queue first;
Draft order / remove controls let you select and reorder or remove a draft.
Choose the exact family: Classic1K, NTAG213, NTAG215 or NTAG216. The existing
single-target append workflow and its eight-draft limit remain separate.

Overwrite and Canonical format start OFF. Arm freezes one to three queued
records and the selected options. Remove every card and explicitly Read
with no card present to open the removal gate. Present a target, Read it,
then Review bulk target. W prepares a review; it never confirms. The
Cancel-default review needs a new, non-repeated confirmation for that
specific target. Every result or canceled target requires another observed
removal. Stop ends an armed job without a background writer or automatic
retry; an uncertain mutation remains visibly unverified until acknowledged.

Actual encoded NDEF/TLV bytes must fit the target's validated CC/MAD area,
including preserved control TLVs. Existing different records or hidden
nonzero data require explicit overwrite permission. Canonical formatting
is limited to fully preflighted factory-key/access Classic1K cards or an
exactly identified, unlocked, unprotected NTAG with an all-zero CC.
It uses fixed NFC keys/access or the part's canonical CC, not arbitrary
metadata writes. It never changes block 0, UID, locks, configuration,
password/PACK or user pages outside the initialized CC area.
Classic4K/partial MAD recovery and existing nonzero CC repair are unsupported.
These workflows have software coverage, not physical formatting acceptance.

Build and test
--------------

From the repository root, with CMake 3.20+, Ninja, a C++17 compiler, Python
3.11+ and SDL2 development files installed::

    python3 devices/cardputerzero/build.py --sanitize --test --smoke

This fetches the exact LVGL 9.5.0 revision in dependencies.lock into
ignored .build storage and builds the desktop simulator. It never installs
packages, connects to readers or deploys to a device. On the validation Mac,
use --cmake /opt/homebrew/bin/cmake to avoid the unrelated broken MacPorts
installation. The helper applies the SDL2-compat/SDL3 runtime library
search path only to its child processes.

Run the preview explicitly::

    .build/cardputerzero-host/quadband-cardputerzero --simulate

A preview build refuses --device. Headless simulation supports --headless,
--seconds N and an optional --screenshot image.ppm. Device operation
requires a Linux framebuffer build and explicit --device. Device paths
can be prefilled with --hf-i2c, --uhf-serial and --ultra-serial; endpoints
remain explicit overrides. --device --detect-only performs only module
identification and prints results without opening the framebuffer/keyboard.

For the self-contained arm64 device build, use an existing Zig 0.15.1
compiler; the helper does not install one::

    python3 devices/cardputerzero/build.py --device --zig /path/to/zig
    python3 devices/cardputerzero/package.py --zig-root /path/to/zig-directory
    python3 -m unittest discover -s devices/cardputerzero/tests -p 'test_*.py'

The output is dist/quadband-cardputerzero_0.3.0_arm64.deb with a SHA-256
sidecar. Packaging rejects a Mac/simulator/dynamic binary or changed
source/artifact hashes. It includes source, notices and internal checksums.
It does not install or publish anything. Its post-install script applies
only the dedicated Grove brightness permission and reloads udev rules;
it does not enable power or start the app. The launcher entry starts a
separate process through an exec wrapper; no autostart service is included.

The device display uses fbdev geometry, stride, offsets and channel
bitfields read from the kernel. Only packed true-color 320x170 RGB565/
32-bit layouts with verified mapped bounds are accepted. The keyboard
device is configurable through --keyboard or the launcher's
APPLAUNCH_LINUX_KEYBOARD_DEVICE override. The standard documented keypad
path is the fallback, not a guarantee for every OS image.

Validation and credits
----------------------

Host ASan/UBSan tests cover fake register/wire devices, POSIX pseudo-terminal
serial framing/deadlines/exclusion, Linux I2C ioctl stubs, actual worker
review/cancel/cleanup behavior, keyboard input and LVGL workflows.
The Linux arm64 executable is cross-built with real Linux headers.
Neither a cross-build nor a desktop rendering proves physical reader,
native keyboard, Grove power/mux, framebuffer or stock-image compatibility.
Do not treat the development package as hardware-validated.

The original project credits to Tototo31 / Ashsh and Midwest Gadgets
branding are retained. Application source is GPL-3.0-only; LVGL is MIT,
Montserrat/Font Awesome fonts carry their included upstream notices, and
the static musl/LLVM C++ runtimes retain their redistribution licenses.
See the root THIRD_PARTY.txt, package notices and source dependency lock.
