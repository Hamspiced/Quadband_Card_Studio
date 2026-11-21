CoreS3 Bluetooth reader hub
===========================

Development firmware for the **M5Stack CoreS3 ESP32-S3** and matching desktop
integration. This is not firmware for the original Basic/Fire or Core2.
Compilation and simulated tests are not evidence of physical reader writes.
Nothing in the build process flashes a device.

Connections and power
---------------------

.. list-table:: Supported attachment layout
   :header-rows: 1
   :widths: 24 24 52

   * - Device
     - CoreS3 connection
     - Notes
   * - Unit RFID or Unit RFID2
     - Port A, I²C
     - One HF unit at 0x28. Supported silicon is detected; both units cannot share that address on one bus simultaneously.
   * - Unit UHF-RFID / JRD-4035
     - Port C, UART
     - 115200 baud, 8N1. The bridge uses the existing M100 protocol and does not change the module's stored baud, region or channel settings.
   * - Chameleon Ultra
     - Main USB-C port, USB host/OTG
     - Data-capable host cable/adapter and suitable independent power. The Chameleon's existing firmware is not replaced.
   * - Card Studio computer
     - Bluetooth Low Energy
     - A single authenticated connection is shared across the three logical readers.

Use the board's Grove connectors rather than assuming that supply voltage
equals UART logic voltage. The CoreS3 board routing uses external I²C SDA2/
SCL1 and Port C TX17/RX18; these are not the pin numbers for every Core variant.
The firmware's board integration owns these resources.

The main USB-C socket cannot simultaneously connect the Chameleon as a device
and the computer as the programming host. **USB host output starts off.**
Disconnect the computer cable and supply suitable independent board power
before using the local USB-host enable action. Never connect two host outputs
together. Use a charged battery or the board's supported independent input,
check the combined RF/USB current budget, and do not assume a small adapter
can power every attached device. No unconditional USB-output preference is
restored on boot.

Browser installer
-----------------

The source includes a browser flasher in ``webflasher/``. Once release
automation and GitHub Pages are configured, the stable installer is served
at https://Hamspiced.github.io/Quadband_Card_Studio/ alongside its matching
firmware files. A source checkout alone does not create that live site.

Use a desktop browser supporting Web Serial, such as Chrome or Edge, over
HTTPS (or localhost for development). Unsupported browsers show manual setup
guidance rather than an install action. Close other programs using the
programming port before connecting.

Before selecting a serial port, confirm the board is physically an M5Stack
CoreS3 ESP32-S3 with 16 MB flash. Basic/Fire, Core2 and other ESP32-S3 boards
are not interchangeable. Chip detection alone cannot prove the board model.
USB host output must be off and the Chameleon/host peripherals disconnected
before connecting the programming computer.

The installer verifies release metadata, firmware download hashes, chip,
flash capacity and supported partition layout before an explicit Install.
It writes the split bootloader/partition/application images at their checked
offsets with no full-chip erase. Supported-layout updates preserve NVS
settings and Bluetooth bonds; incompatible existing layouts are refused
rather than silently migrated. A blank flash can receive the initial layout.

Do not disconnect power or close the tab while programming. Interruption or
verification failure is reported as incomplete, not success; reconnect and
explicitly start a fresh installation when ready. No automatic whole-install
retry or erase-all control is provided. After verified installation the
board is reset and the serial port is released.

The page displays desktop release, firmware version and source commit.
Development snapshots are labelled explicitly and cannot be deployed as a
stable release through the Pages staging checks. Browser simulation tests
are not evidence of a successful physical Web Serial flash.

Build the firmware
------------------

Use the pinned native ESP-IDF 5.5.3 toolchain. The firmware helper downloads
the exact dependency revisions listed in its lock into an ignored local
build directory; vendor SDKs are not copied into public source archives.

From the repository root after activating ESP-IDF:

.. code-block:: sh

   python3 firmware/cores3/build.py

To package the verified build with source, dependency notices and checksums,
keep that SDK activated and run:

.. code-block:: sh

   python3 scripts/package-cores3.py

The local bundle is ``dist/Quadband-CoreS3-development.zip``. It contains
the split application/bootloader/partition files and generated flash
arguments, not an instruction to flash the application binary at offset zero.

See `firmware instructions <../firmware/cores3/README.rst>`_ for toolchain
activation, host tests, generated artifacts and manual flashing guidance.
Firmware builds use at most four compilation jobs. No automatic device
discovery/flashing or physical card writes are part of the build.

After a manual flash, disconnect the programming computer before enabling
the CoreS3 host port for Chameleon. Keep the bootloader, partition table,
application binary and flash arguments together with the dependency lock
and checksums.

Touch dashboard and operating modes
-----------------------------------

Firmware 0.2.0 boots to a branded **Home** screen. Choose **Connected** to give
Card Studio access to its readers, or **Standalone** for local ownership.
The desktop sidebar displays the board mode and pauses reader access during
handoffs. Returning to Connected requires explicit reader reselection.

The 0.2.1 visual refresh adds deeper dark surfaces, high-contrast teal accents,
rounded controls, proportional antialiased type and workspace attachment
indicators. Safety controls and operating-mode behavior are unchanged.

Firmware 0.3.0 replaces the informational reader pages with working
on-device UHF, HF/NFC and LF tools. Touching a reader opens its workspace;
scanning, reading and mutations require explicit actions. All session data,
drafts and source captures remain in RAM. File/SD browsing and persistent
profile import/export remain unavailable.

Settings provides dark/light appearance, a bounded pairing window, confirmed
bond reset and confirmed USB-host authorization. Theme and bonds are saved;
USB authorization, operating mode, card contents and supplied credentials
are not. Diagnostics shows heap, battery, request and module state.

Leaving an active mode while paired or busy requires confirmation. A handoff
blocks new reader requests, waits for the current physical transaction,
releases RF and checks Chameleon mode restoration before granting a new owner.
**Stop** in Connected safely returns to Home. An unverified USB release instead
leaves the board in Home with both owners disabled and a restart warning.
Pairing approval takes priority and requires a fresh press after its exact
challenge is displayed.

Standalone HF / NFC
--------------------

Attach one Unit RFID or Unit RFID2 to Port A; the known module is detected
automatically. Both use the same guarded local workflows:

* **Memory → Read card** identifies Classic 1K/4K or NTAG213/215/216 and
  reads available blocks/pages. Partial reads are shown as partial, not
  complete sources. Select a unit with arrows or its number.
* **Auth** selects Classic Key A/B and a supplied six-byte key. The factory
  key button is explicit. Standard public NFC keys are a separate opt-in
  mode, not key recovery. NTAG authentication permits an explicitly supplied
  password and optional expected PACK; protection settings are not changed.
* **Edit bytes** opens the hex keyboard for exactly one 16-byte Classic
  data block or four-byte NTAG user page. **Review edit** shows the expected
  target and desired bytes; only the explicit **Write** button can submit it.
  Identity, previous data, permissions and generation are checked again.
  UID/manufacturer data, trailers, lock/configuration/password pages remain
  protected.
* **NFC** shows decoded record summaries and a bounded RAM draft queue.
  The 19 existing record builders are available through paginated forms:
  text/URI/link variants, search, application, mail, contact, phone,
  location, SOS, Bluetooth, printing/OpenSpool, Wi-Fi and custom MIME data.
  Fields are bounded to 256 bytes; the queue holds at most eight new
  records and must fit the actual card. Review appends queued records
  while preserving existing records/identifiers, commits message length
  last and verifies the final message. **Clear draft** does not erase the
  tag. Wi-Fi credentials entered here are intentionally stored in that
  record on the tag.
* Classic NFC writes require an existing valid MAD/MAD2 allocation and
  suitable keys. A blank Classic card is not silently formatted. Known
  all-NULL NTAG memory can receive its first NFC message; unknown reserved
  layouts stay read-only.
* **RAM copy** captures complete eligible source data without UID, keys,
  trailers, MAD directories or NTAG metadata. Remove the source and
  **Read card** with no card present to arm the removal gate, then read a
  same-family target and review the copy. Every outcome resets the gate.
  This copies data at the existing unit addresses, not card identity,
  protection or a new Classic NFC allocation.

Local HF uses the Grove RFID/RFID2 in this milestone. The desktop-connected
Ultra Classic-HF path remains supported; a local USB-HF backend is not
offered as a substitute for the Grove reader.

Bulk NDEF and canonical formatting — development 0.4.0
------------------------------------------------------

The NFC workspace adds a separate bulk job using one to three frozen RAM
drafts. Select the exact Classic1K/NTAG213/215/216 family and choose
overwrite/formatting explicitly; both options default off. The existing
single-target append editor and Classic4K support remain available.
Use the draft controls to reorder or remove records before arming.

After arming, remove all cards and explicitly Read with no card present.
Read a target and prepare its review. Only a fresh explicit Confirm can
write that individual target; polling or merely presenting a card cannot.
After success, rejection or cancellation, observe removal again. Transport
and authentication errors are not removal. Stop disarms a job; unverified
writes latch a visible warning and are never automatically retried.

Fit checks use encoded bytes plus actual target CC/MAD/TLV overhead, not
physical memory size. Preserved control TLVs reduce capacity. Existing
different records and hidden nonzero data require overwrite authorization.
The fixed canonical formatter requires complete factory-key/access
Classic1K preflight or exact unlocked NTAG identity with an all-zero CC.
It does not repair Classic4K, partial MAD layouts or nonzero OTP CC values.
Ordinary data writes still cannot edit protected metadata; only the narrow
formatter can write its fixed Classic trailers or canonical CC. UID,
manufacturer data, locks, configuration, password/PACK and NTAG data
outside the initialized CC area remain unchanged.

Connected desktop clients require the separate ``classic1k_format`` or
``ntag_initialize`` capability for bridge operations 22/23. Old firmware
and unsupported readers reject those operations rather than using a raw
write fallback. Full request snapshots bind the reviewed target; mutations
retain previous-byte, identity/generation checks and final verification.
No physical formatting or 0.4.0 flash is established by host tests.

Standalone UHF
---------------

Attach the supported JRD-4035/M100-family module on Port C.
**Start scan** performs bounded single inventory polls until **Stop scan**.
The list holds up to 64 EPC/PC/RSSI/read-count entries, with pages of four.
Stop inventory before selecting a tag for memory access.

**Memory → Settings** selects Reserved/EPC/TID/User bank, a 16-bit starting
word, 1–64 read words and an optional supplied access password. All returned
bytes can be paged through. EPCs longer than 28 bytes are inventory-only
because exact selection is limited to PC plus a 28-byte EPC.

User edits require a matching prior read of 1–32 words. EPC rename reads the
full overwritten old/new range, preserves PC control bits, reselects the
exact expected tag and verifies the new EPC. Reserved/TID writes, lock/kill,
region changes and CRC-word writes remain disabled. **RF power** reads the
current setting and reviews a change only within 18–26 dBm, then reads it
back. Unsupported buzzer operations are not invented.

Standalone LF / Chameleon Ultra
--------------------------------

Enable USB host through confirmed Settings only with the programming
computer disconnected and suitable independent power, then attach the Ultra.
Available formats come from its actual official 2.x command list:
EM410x/Electra, HID Prox, Viking, ioProx, PAC/Stanley, Jablotron and Pyramid
when reported by that firmware. HID Prox and Pyramid are read-only here.
The HID format hint is editable.

**Scan LF** reads an identity; **Use scanned ID** explicitly captures it as
the desired RAM source. **Custom ID** edits supported IDs or selects the
editable EM410x test ID ``0102030405``. Remove cards and scan with no card
present to arm target placement, then present the intended target and
review the write. Transport errors are not treated as an absent card.

Writes require the matching scan command and password-free block command
3016. EM410x/Electra, Viking, ioProx, PAC/Stanley and Jablotron use the
existing tested encoders. Only one intended unprotected T5577 target may be
on the antenna. An unreadable/absent identity is not proof that a tag is
blank, compatible or unprotected. Data blocks are written before
configuration, the PWD bit stays clear, and block 7, passwords and lock bits
are never written. Verification scans the desired format and compares the
identity. Original Ultra mode is restored and checked on release.

Stop, privacy and incomplete writes
------------------------------------

Local jobs and desktop operations share one physical worker. Standalone
rejects desktop peripheral work even if BLE stays connected. Local scans
are not forwarded to that desktop. Stop/Back/mode handoff cancel between
physical commands, drain the current reply and release RF. A partly changed
target remains **write unverified** through Stop cleanup; read it again
before another review. No write is replayed automatically.

Back/Clear session or leaving local ownership removes applicable RAM
results, drafts and credentials. Saved Bluetooth bonds and appearance are
separate. File storage and automatic card-data persistence are not enabled.

Pair and select readers
-----------------------

1. Power the CoreS3 with the appropriate peripherals attached. Detection
   queries identify devices; they do not automatically inventory RF tags or
   modify cards.
2. Choose **Connected**, then open the pairing window in **Settings**. In Card Studio's CoreS3
   sidebar panel, scan and select the advertised board.
3. Confirm the displayed code and local approval/system pairing prompts.
   There is no fallback to an unencrypted reader channel.
4. Select the detected Grove HF unit in HF Workspace, the CoreS3 UHF module
   in UHF Workspace, or the bridged USB Chameleon for its HF/LF workflows.
   Missing endpoints are unavailable, not replaced with another reader.
5. Enable USB host locally only after the programming cable is disconnected
   and power is suitable; then attach the Chameleon with a data-capable
   OTG arrangement and wait for identification.

Peripheral generations and a fresh connection nonce bind requests to their
actual device/session. Removing a module invalidates affected operations and
write previews. Reconnection does not replay a queued write. A timeout after
transmission means the card may have changed; read it again rather than
assuming it stayed unchanged.

The Chameleon has one shared HF/LF lease. Do not run another application
against it while the hub owns it. Normal release attempts to restore its
previous device mode; abrupt power loss cannot guarantee restoration.

HF cards and profiles
---------------------

Classic 1K retains its existing 16-sector geometry and legacy profile format.
Classic 4K has 40 sectors, with sixteen-block sectors from sector 32 onward.
Data writes validate the actual access groups and preserve manufacturer data,
keys and trailers. NDEF uses existing valid MAD/MAD2 allocation; the bridge
does not silently format a blank Classic card or change its keys.

NTAG213/215/216 use explicit physical layouts. User-page and NDEF writes
check identity, previous bytes, CC access, static/dynamic locks and supplied
authentication where available. The NDEF message length is committed last
and read back. Unknown or overlapping reserved-memory layouts remain
read-only. UID/manufacturer pages, CC, lock bytes, password/PACK and
configuration pages are never write targets.

``.qcard`` version 1 Classic 1K files remain compatible. New version 2
profiles explicitly identify family and geometry and contain only eligible
data blocks/pages. A Classic 1K profile is not reinterpreted as Classic 4K
or NTAG, and importing a profile does not transfer keys, UID or protection
settings. ``.qlf`` remains the separate LF identity format.

UHF and USB Chameleon
---------------------

UHF operations reuse inventory, EPC/User-memory verification and conservative
18–26 dBm power controls. The existing exact-selection and module write-size
limits still apply. Reserved/TID writes, lock/kill, firmware updates and
region changes are not forwarded. A missing buzzer command is not simulated
by beeping the Core itself.

The USB path supports the application's permitted Chameleon HF/LF commands,
with identity/data guards and the password-free T5577 policy. It is not an
unrestricted command tunnel. NTAG support in the initial bridge is through
the Grove HF module; arbitrary ISO14443 forwarding, emulation-slot changes,
key recovery and Chameleon firmware updates are not exposed.

Validation
----------

Shared protocol, geometry, profile and lock-map examples are in
``protocol/fixtures/cores3-v1.json``. They include synthetic test identities
and data, not exported real cards. Run the hardware-free contract tests with:

.. code-block:: sh

   python3 -m unittest discover -s protocol -p 'test_*.py' -v

Final physical acceptance needs the real CoreS3, each HF unit, UHF module,
USB/power arrangement, supported Chameleon firmware and intended expendable
cards. Test pairing, removal/replacement, cancellation and read-back failures
as well as successful reads. Do not call the hub hardware-verified based on
compilation or simulated readers alone.

`Wire contract <../protocol/cores3-bridge-v1.rst>`_
· `Validation status <VALIDATION.txt>`_
· `Firmware instructions <../firmware/cores3/README.rst>`_
