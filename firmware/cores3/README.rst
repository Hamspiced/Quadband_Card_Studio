CoreS3 reader bridge firmware
=============================

Native ESP-IDF 5.5.3 application for M5Stack CoreS3 ESP32-S3.
The shared version-1 wire contract is ``../../protocol/cores3-bridge-v1.rst``.
The 0.4.0 bulk-NDEF build includes the preceding partial-redraw update; it has not been flashed
or visually verified on hardware. Version
0.3.0 was flashed on request with split-image hash verification and NVS
preserved. Serial startup confirmed 0.3.0, detected 8 MB PSRAM, passed its
memory test and reached board/BLE initialization with USB host output off.
The preceding 0.2.0 dashboard was visually confirmed by the user.
Reader operations, BLE pairing, the browser-flashing path and an OTG power
arrangement remain unverified.

Connections and power
---------------------

* Connect **one RFID OR RFID2** to Port A. Both use I2C address ``0x28``.
  The external hardware I2C bus is ``M5.Ex_I2C``; board API values are checked
  against SDA GPIO2/SCL GPIO1. Do not use the internal display/power I2C bus
  or a software I2C adapter.
* Connect M5Stack UHF-RFID/JRD-4035 to Port C, 115200 baud, TX GPIO17 and
  RX GPIO18. These are also checked against the board API.
* Connect an official Chameleon Ultra 2.x CDC-ACM device to the main USB-C
  port using a data-capable OTG cable. The host opens VID/PID
  ``6868:8686``, communication interface 0, 115200 8N1, DTR/RTS asserted.
  Version, Ultra model, command list and original mode are queried before
  announcing the device; missing commands are not invented.

The main USB port cannot simultaneously host Chameleon and connect to a
programming computer. **Disconnect the PC and use battery/DinBase power
before enabling host mode.** USB output starts OFF on every boot. In
**Settings**, select **USB host**, read the power warning and explicitly
confirm authorization for this boot only. An active or queued reader operation
blocks authorization. Firmware refuses authorization when the PMIC reports
an external VBUS above 1 V or cannot read VBUS. It then installs the native USB host and
CDC-ACM driver, enables ``M5.Power.setUsbOutput(true)`` and checks its output
state. Authorization is not saved. Confirm actual voltage, cable polarity,
available current and reader stability on the target hardware.

Pairing and operation
---------------------

Use **Settings** to open a 60-second pairing window. Numeric comparison
or a displayed passkey requires the explicit **Approve** button on the
pairing dialog; all other touches are ignored except **Reject**. Compare the
displayed code with the desktop; expiry rejects the request. Bond reset is
a separate confirmed Settings action. Only one connected client and one
stored bond are configured. Legacy pairing/Just Works is not an accepted
security fallback: operation requires encrypted, MITM-authenticated,
16-byte-key, bonded state.

The dedicated RX characteristic is write-with-response and encrypted/
authenticated; the dedicated TX characteristic uses indications. A TX
subscription is mandatory before HELLO. Indications are fragmented
conservatively at 20 bytes and acknowledged one at a time. Four outgoing
frames and one physical work item are the bounded queues; overflow closes
the link rather than silently losing a write result.

The touch dashboard provides branded dark/light screens, three independent
peripheral slots, activity, settings and diagnostics. Appearance alone is saved
alongside Bluetooth bonds; card contents and supplied credentials are not
persisted. The board boots to **Home**, with neither reader owner enabled.
Choose **Connected** to permit desktop reader requests, or **Standalone** for
local ownership. A desktop BLE session may remain attached for status/control
in either mode, but peripheral work is allowed only in Connected.

Standalone provides real Grove HF memory/NFC tools, UHF inventory and selected
memory/EPC tools, and capability-driven USB Chameleon LF tools. Touch input,
read results, review-before-write and verification run on-device without a
desktop. Selecting Standalone alone does not start RF work; choose a reader
and explicitly start a scan/read. Results, source captures and drafts are
RAM-only. MicroSD/file browsing, import/export and persistent profiles are
not implemented in this milestone.

See ``../../docs/CORES3.rst`` for the on-device workflow and limits.

Leaving an active mode while paired or busy requires confirmation.
Switching blocks new reader requests, stops queued work between physical
commands, waits for the current physical transaction to finish, releases RF
and attempts original Chameleon mode restoration before changing ownership.
All reader generations then advance; desktop selections must be made again.
**Stop** in Connected requests a safe return to Home. A pairing dialog always
takes priority over other confirmation dialogs. A newly displayed dialog
discards any existing touch and requires release followed by a fresh press;
approval is bound to the exact displayed pairing challenge and processed
on the Bluetooth host event queue.

Chameleon restoration checks command status and reads back the original mode.
If release cannot be verified, firmware falls back to Home, quarantines USB,
disables both owners and displays a restart warning instead of claiming a
successful handoff. Optional USB initialization failures keep output off and
report Failed without aborting the dashboard; restart after checking power
and connections.

The protocol document defines status numbers. Disconnect invalidates the
desktop session, zeroes receive/
queued buffers, cancels future physical commands and schedules RF cleanup/
original Chameleon mode restoration after an in-flight desktop transaction
has finished. In Standalone, BLE disconnect preserves the local job and
queue; remote CANCEL cannot stop a local operation. Local inventory reports
are routed to the on-device workspace, never to a connected desktop.
A new session cannot revive old work or accept old results.
An already transmitted write is never retried automatically.

Implemented policy
------------------

RFID2 is recognized only by WS1850S fingerprint ``15``; RFID is recognized
by MFRC522 fingerprints ``91`` and ``92``. Other responding silicon is
reported unknown without usable capabilities. Detection only reads module
identity and initializes known drivers, then disables the RF field.

The Grove path implements identify, Classic 1K/4K authenticated block reads,
protected data-block compare/write/read-back, NTAG213/215/216 page reads and
protected user-page compare/write/read-back, explicit supplied PWD_AUTH
with optional PACK checking, and authentication reset. Writes require the
expected UID; Classic block 0 and all trailers are forbidden. Access-bit
redundancy and actual supplied key permissions are checked before mutation.
Large Classic sectors use offsets 0--4, 5--9 and 10--14 as access groups.
NTAG checks real geometry, exact known CC mapping/access, static/dynamic
locks and AUTH0. Metadata, CC, UID, configuration, lock, password and PACK
pages are never writable. NTAG215/216 dynamic lock bits span 16 pages;
NTAG213 bits span two pages. Passwords are supplied explicitly, kept only
in transient RAM, not guessed or stored.

UHF raw transactions validate frame type/checksum, allowlist, payload size,
bank/address/count and power limits. Reads permit 1--64 words; writes permit
1--32 words only in User or EPC after CRC. A write additionally requires
an acknowledged exact PC+EPC selection, confirmed selection read-back and
active selected mode. Firmware re-queries selection and reads current PC
before forwarding the single write. Raw inventory notifications retain the
M100 frame and generation, including coalesced reports. Lock/kill, region
changes and Reserved/TID writes cannot be forwarded.

USB commands are checked against both the bridge whitelist and the real
device command list. Classic writes require a preceding same-card,
same-key/block read cached in the current session; firmware freshly scans
UID/ATQA/SAK geometry, reads the trailer/access bits and old block, forwards
one write and verifies it. LF and HF use the same serialized worker, and
changing mode or using LF invalidates the HF cache. Command 3016 accepts
only password-free blocks 0--6, exactly 11 payload bytes, final six bytes
zero and configuration PWD bit clear. All legacy password-setting LF
writers, emulation, flashing and raw ISO14443 forwarding are rejected.
USB NTAG writes are deliberately not implemented or advertised.
Native USB reply deadlines are bounded at 2.5 seconds for metadata/HF and
4.2 seconds for LF commands, leaving response-delivery headroom under the
desktop's five-second LF timeout.

The standalone service layer also validates NDEF/MAD/permissions and
multi-unit preflight. Every physical typed write still undergoes the shared
firmware unit-level guard. Desktop file/profile formats retain their existing
meaning; RAM-only local captures are not automatic file exports.

Development 0.4.0 adds separately reviewed one-to-three-record bulk jobs and
fixed-output factory Classic1K/blank-CC NTAG initialization. Generic unit
writes keep their trailer/CC exclusions. Optional bridge capabilities
``classic1k_format`` and ``ntag_initialize`` gate operations 22/23, each bound
to a complete expected readable snapshot. See the canonical-formatting
extension in the protocol and the workflow/limits in ``../../docs/CORES3.rst``.
Classic4K/partial-MAD recovery, arbitrary CC upgrades, lock/config/password
changes and automatic write retries are not provided.

Build and software validation
-----------------------------

Install/activate ESP-IDF **5.5.3**, then run from the repository root::

    . /path/to/esp-idf/export.sh
    python3 firmware/cores3/build.py
    python3 firmware/cores3/tests/run.py
    python3 firmware/cores3/tests/ui_run.py
    python3 firmware/cores3/tests/standalone_run.py

The helper fetches only the exact revisions in ``dependencies.lock`` into
ignored ``.build/deps/``. This generated lock is the firmware's pinned Git
dependency manifest, not an ESP Component Manager resolver file. Component
Manager is disabled so upstream manifests that name moving ``main`` branches
cannot override those pins. No Arduino runtime or vendored hardware SDK is
used. IDF configures the project; Ninja builds with at most **four jobs**.

The defaults select CoreS3 QSPI PSRAM, not octal PSRAM, and allow an internal-RAM
fallback when PSRAM is unavailable. Diagnostics reports the actual free heaps.
The dashboard and standalone tools compose changed regions in a fixed
320 by 16 RGB565 band buffer (10,240 bytes), then transfer completed bands
instead of showing erase/draw steps on the LCD. Unchanged standalone visual
output does not redraw, even when the worker publishes a new revision.
Overlay restoration and new write-review tokens force repainting. Rendering
does not require a full-screen framebuffer or a PSRAM allocation.

On the validation workstation, MacPorts CMake was broken due to a missing
libxml2 dylib. The already installed Homebrew CMake worked after setting
``PATH=/opt/homebrew/bin:$PATH`` following SDK activation. No system library
was replaced. The tested SDK HEAD was
``2c211b236707889e8400c4dc5644dd5c4ee071e0``; its pre-existing dirty submodule/
example status is recorded in ``.build/artifacts.json`` and was not changed.

Artifacts are under ``.build/idf/``:

* ``quadband_cores3.bin`` and ``quadband_cores3.elf``;
* ``bootloader/bootloader.bin``;
* ``partition_table/partition-table.bin``;
* ``flash_args`` and ``flasher_args.json`` (inspect before any manual flash).

``.build/artifacts.json`` records artifact SHA-256/size, exact source-file
hashes, dependency pins, SDK revision/status and ``hardwareValidated=false``.
The build helper never flashes, erases or opens a serial device. If a later
manual flash is authorized, disable host output, disconnect Chameleon and
use the generated flash arguments with the programming connection.

Host tests compile the same ``main/bridge.cpp`` with C++17, warnings-as-errors,
AddressSanitizer and UndefinedBehaviorSanitizer. They test every split of all
golden frames, coalescing, CRC/header/buffer rejection, session admission,
stale generation, duplicate IDs, busy/wrap/disconnect, Classic geometry/
access, NTAG family/lock/auth/CC boundaries, M100 inner framing and guards,
and T5577 restrictions. Shared profile/legacy/UUID fixtures are also checked.
Use ``--protocol /absolute/path/to/protocol`` for a separate worktree.
Tests never access hardware.

Mode tests exercise real session admission, blocked owners, confirmation,
pending transitions, release failure, displayed-challenge matching and the
fresh-press guard. UI sanitizer tests exercise navigation, modal priority,
every modal pixel, unavailable services, USB states and redraw bounds.
Use ``ui_run.py --png`` with PySide6 installed to render the production
commands and shared Studio Sans coverage glyphs. The 0.2.1 visual refresh
uses deeper dark surfaces, restrained saturated accents, rounded panels,
proportional antialiased text, measured truncation and distinct Stop styling.
It does not change touch regions, mode ownership or power/pairing guards.
The renderer stays allocation-free and does not require a framebuffer.
``tools/ui_fonts.py`` regenerates its ASCII font assets from the pinned
M5GFX DejaVu24/DejaVu40 glyphs using 2x2 coverage resampling. Upstream notices
are retained in ``main/ui/LICENSE-DejaVu.txt`` and the firmware bundle.

Dependency and source notices
-----------------------------

Firmware version is ``0.3.1`` on repository baseline ``c2a0afa`` plus the
reviewed firmware source patch; exact compiled source hashes are in the
build provenance file. All dependency revisions below were compile-tested,
not physically tested:

* M5Unified ``44952cb8e4ac88bf04ad23b9dfd6f4b05c788348`` (MIT).
* M5GFX ``7cb26b44ab31bb41a0a597196dd2c6d868869a21`` (MIT; preserve the
  upstream font/third-party notices when redistributing binaries).
* M5Unit-RFID ``4b98e658181d6f0a7c3dc2199302ccc510970193`` (MIT).
* M5Unit-NFC ``471a6e02e8052264d39b3ac97005ce4de9d1cb91`` (MIT).
* M5UnitUnified ``68a8fa74959a3c18ffae5ce7f73ac61ecf651d4c`` (MIT).
* M5Utility ``336195d0f9466332552b337b94cab72f83c8d2ca`` (MIT).
* M5HAL ``e3a84320f21bb48062f8101301020e946182a71d`` (MIT).
* Espressif esp-usb ``7bf402fe68aa310d4489cc65fe801c5fbcc0454b``,
  ``host/class/cdc/usb_host_cdc_acm`` (Apache-2.0).
* ESP-IDF/NimBLE and linked libraries retain their upstream licenses and
  notices. Dependency source URLs are preserved in ``dependencies.lock``;
  source/licenses remain in the fetched repositories, not this patch.

Hardware acceptance still required
----------------------------------

Verify pairing denial outside the local window, numeric comparison,
reconnection with a bond, bond reset, indication subscription/backpressure,
safe USB power interlock and actual Chameleon CDC descriptors. Exercise
both known HF modules, Classic 1K/4K large-sector boundaries, supplied
password/PACK errors, all NTAG lock/protection boundaries, real UHF no-tag/
multiple-tag responses, removal/replacement, cancellation and disconnect
during verification. Use expendable owned cards only after explicit
authorization. Never interpret compile or host-test success as verified RF,
power, USB interoperability or successful physical writing.
