Quadband Card Studio by Midwest Gadgets
=======================================

One local workbench for magnetic-stripe, HF/NFC, UHF and LF cards.
Native SwiftUI on macOS, with a Python/Qt desktop application for Windows and Linux.

**Release: v1.2.1** · Application version **1.2.1** · **GPL-3.0-only**

`Download the latest release <https://github.com/Hamspiced/Quadband_Card_Studio/releases/latest>`_
· `Detailed user guide <docs/USER-GUIDE.txt>`_
· `Windows/Linux setup <desktop/README.txt>`_
· `Validation status <docs/VALIDATION.txt>`_
· `Release automation setup <docs/RELEASING.txt>`_

This release adds Chameleon Bluetooth connection parity while retaining the
CoreS3 reader hub, Classic 4K/NTAG workflows and reviewed bulk NDEF encoding.
Component versions are desktop 1.2.1, CardputerZero 0.4.0 and CoreS3 0.5.0.
Device/reader hardware acceptance is still required; software validation
and package availability are not claims of physical write compatibility.
See `CoreS3 setup <docs/CORES3.rst>`_ and the
`bridge protocol <protocol/cores3-bridge-v1.rst>`_.

**CardputerZero development port:** a separate keyboard-first Linux application
now reuses the standalone HF/NFC, UHF and LF engines. It targets the Zero's
320x170 display and ships as an arm64 Debian package, not ESP32 firmware.
See `CardputerZero build, connections and limits <devices/cardputerzero/README.rst>`_.
Host simulation and cross-build validation do not establish physical reader
or stock-OS compatibility.

Chameleon BLE connection parity covers desktop
1.2.1, CardputerZero 0.4.0 and CoreS3 0.5.0. Desktop keeps its existing native
BLE transports with fresh device selection and disconnect diagnostics; the
Zero uses an unprivileged BlueZ helper, and CoreS3 adds a selected-reader
central link alongside its desktop bridge. USB remains supported. Pairing,
validated readiness and card operations are separate steps; no automatic
write replay or USB/BLE fallback is introduced. Package availability and
software tests do not establish physical pairing compatibility.

Compatible hardware
-------------------

The table lists implemented reader paths, not a guarantee that every model,
firmware revision or operating system has been physically tested. Physical
evidence below is from macOS; Windows/Linux reader hardware still needs
target-system validation. **Physical card writes remain unverified.**

.. list-table:: Readers and connections
   :header-rows: 1
   :widths: 18 12 18 28 24

   * - Hardware
     - Workspace
     - Connection
     - Available operations
     - Validation / limitations
   * - MSR X6
     - Magstripe
     - USB HID or supported FF00 Bluetooth
     - Read, write, second-swipe verification, coercivity and density settings
     - Physical BLE handshake confirmed; writes unverified. Bluetooth Classic-only variants are not covered.
   * - MSR605
     - Magstripe
     - USB-to-serial; normally 9600 baud
     - Read, write and second-swipe verification
     - Simulated serial tests; physical reader unavailable.
   * - MSR605X
     - Magstripe
     - USB HID
     - MSR-compatible read/write workflows
     - Protocol-based support; physical model/firmware validation required.
   * - MSR600
     - Magstripe Read
     - USB keyboard mode
     - Focused swipe capture, track markers and configurable marker-free mappings
     - Parser and keyboard-capture tests; physical reader validation required. Read-only.
   * - ACS ACR122U
     - HF/NFC
     - USB PC/SC
     - Classic 1K reads, NDEF records, data-block workflows and reader controls
     - Physical 64-block/NDEF reads and reader-setting reads confirmed; writes unverified.
   * - ACS ACR1252U
     - HF/NFC
     - USB PC/SC
     - Compatible ACS Classic 1K card-memory commands
     - Protocol-based support; physical validation required. Advanced reader controls are not exposed for this model.
   * - ACS ACR1552U
     - HF/NFC
     - USB PC/SC; compatible ACS driver for reader controls
     - Classic 1K workflows, firmware-gated reader settings and RF presets
     - Simulated tests; physical reader unavailable. Feature availability depends on firmware and driver.
   * - Piswords UHF2 / UCM60x
     - UHF
     - USB serial; supplied UCM601 uses 9600 baud, address 0
     - Inventory, memory reads, verified-write workflows for EPC/User memory, buzzer and RF power
     - Physical UCM601 inventory and EPC/TID/PC reads confirmed; writes unverified.
   * - M5Stack JRD-4035
     - UHF
     - USB-to-TTL serial adapter; normally 115200 baud, 8N1
     - Inventory with RSSI, memory reads, verified EPC/User-write workflows and RF power
     - Manufacturer-packet and serial-simulation tests only; physical module operation unverified.
   * - YRM100
     - UHF
     - USB-to-TTL serial adapter; normally 115200 baud, 8N1
     - M100-family inventory, memory reads, verified EPC/User-write workflows and RF power
     - Manufacturer-packet and serial-simulation tests only; older firmware may use 9600 baud.
   * - Chameleon Ultra
     - LF and HF/NFC
     - USB serial or Nordic UART Bluetooth
     - Supported LF identity scans, password-free T5577 workflows and Classic 1K HF workflows
     - Physical USB identification and limited LF/HF checks confirmed; full HF reads and physical writes unverified. BLE pairing remains platform/firmware dependent.
   * - M5Stack CoreS3 reader hub
     - HF, UHF and LF
     - One paired BLE connection to Card Studio
     - Exposes detected Grove HF/UHF units and a USB-hosted Chameleon Ultra
     - Development firmware and simulated integration tests; physical hub operation requires validation.
   * - M5Stack Unit RFID / RFID2
     - HF/NFC through CoreS3
     - Port A I²C; one unit at address 0x28
     - Known Classic 1K/4K and NTAG213/215/216 user-memory/NDEF workflows
     - Firmware detects supported MFRC522/WS1850S variants. Unknown layouts remain read-only.

Chameleon Lite is not supported because it lacks the required reader hardware.
Unrelated HID UHF readers, proprietary reader Bluetooth links and arbitrary
serial-reader protocols are not automatically compatible.

Supported cards and tags
------------------------

.. list-table:: Card and tag scope
   :header-rows: 1
   :widths: 24 38 38

   * - Card / tag family
     - Implemented support
     - Important limits
   * - ISO magnetic-stripe cards
     - Up to three tracks through compatible MSR readers; HiCo/LoCo and density selection
     - Writes require a second swipe to verify. MSR600 keyboard mode is read-only.
   * - MIFARE Classic 1K
     - NDEF records, 47 ordinary data blocks, local credential profiles and batch data copying
     - Normal writes preserve manufacturer data, keys and trailers. Explicit canonical formatting is restricted to fully preflighted factory-key/access cards and changes the standard NFC keys/access layout.
   * - Gen1a Magic Classic 1K
     - Normal authenticated Classic 1K data access
     - No backdoor/UID-changing commands; not a full UID/key clone.
   * - MIFARE Classic 4K
     - Geometry-aware reads, ordinary data writes, MAD2 NDEF, family-matched profiles and copying
     - 40 sectors/256 blocks; manufacturer block and every trailer stay protected. Existing valid formatting and keys are required.
   * - NTAG213 / NTAG215 / NTAG216
     - Supported PC/SC and Grove HF page reads, guarded user-page writes, NDEF and family-matched profiles
     - Exact identity, CC, locks and authentication are checked. Only a separately reviewed all-zero CC initialization can change CC; UID, locks, password/PACK and configuration remain protected.
   * - EPC Gen2 / ISO 18000-6C UHF
     - Inventory, EPC/TID/User/Reserved reads, EPC and User-memory write workflows
     - No Reserved/password or TID writes, lock/kill operations or firmware flashing.
   * - LF identities and writable T5577 targets
     - Firmware-dependent scans; password-free EM410x, Electra, Viking, ioProx, PAC/Stanley and Jablotron encoders
     - Writes require Chameleon command 3016, an unprotected writable target and read-back verification. No password-block writes or PWD enablement.
   * - HID Prox and Pyramid LF
     - Scan and save supported identities when reader firmware provides the format
     - Read/save only in this password-free implementation.

DESFire application-file editing and arbitrary card-management commands are
not implemented. PC/SC NTAG support requires the reader to return the exact
supported GET_VERSION response; the selected family is not an identity override.
Grove HF supports the implemented CoreS3/Zero adapters. Do not assume a
Chameleon connection supports NTAG or canonical formatting.
Unrecognized cards can be identified without enabling unsupported writes.

M5Stack / YRM100 adapter setup
------------------------------

Select the matching reader profile in **UHF Workspace**, then select the
USB-to-serial adapter's port.

* Use TTL UART logic levels matched to the module, not RS-232 voltage signaling.
* Cross adapter TX to module RX and adapter RX to module TX; share ground.
* Provide suitable module power. Supply voltage and UART logic voltage are
  separate requirements; do not assume the adapter can power the RF stage.
* Default to **115200 baud, 8N1, no flow control**. Older M100 firmware may need
  **9600 baud**. A controller's USB debug port requires a transparent binary
  UART bridge; a direct USB-TTL adapter does not.
* M100 memory access uses exact PC-plus-EPC selection for **2–28-byte EPCs**.
  Longer EPCs are inventory-only. Limits are **16-bit word addresses**,
  **64 words per read**, **32 words per write**, and conservative
  **18–26 dBm** power controls.
* Module profiles have no documented buzzer control. Region/channel settings
  remain unchanged, and frequency is shown as not reported when absent.

See the `detailed guide <docs/USER-GUIDE.txt>`_ for the M5Stack Grove pinout,
module-specific limits and reader workflows.

Features
--------

* Separate Magstripe Write/Read, UHF, HF and LF workspaces.
* Persistent **System / Light / Dark** appearance selection.
* Magstripe connection controls and a shared Chameleon USB/Bluetooth panel
  in the sidebar, with UHF first under Contactless.
* NDEF editor with 19 record types, including text, links, contacts, Wi-Fi,
  Bluetooth, application records, OpenSpool and custom MIME data.
* Bulk NDEF jobs freeze one to three records and the exact target family.
  Each target requires observed removal, a fresh Cancel-default review and
  explicit confirmation. Overwrite and canonical formatting default off.
  Capacity is checked against serialized bytes and actual target CC/TLVs;
  an uncertain write stops the job without automatic retry.
* Local ``.qcard`` data profiles, write-from-file and **Copy to infinity**
  batches with removal detection and per-card results.
* Staged write review, identity checks, protected-block restrictions and
  read-back verification. Writes are never automatically retried.
* Local-only card data, explicit exports and session clearing.
* Development CoreS3 BLE hub with peripheral detection, generation-bound
  operations, authenticated pairing and family-aware Classic 4K/NTAG data.
* CoreS3 0.3.0 standalone Grove HF/NFC, UHF inventory/memory/EPC and Ultra LF
  tools with touch input, RAM source capture, reviewed writes and verification.

Download and run
----------------

**macOS:** download ``Quadband-Card-Studio-1.1.1-macOS-universal.zip`` from the
`release page <https://github.com/Hamspiced/Quadband_Card_Studio/releases/latest>`_,
extract it and move the app into Applications. The universal app targets Apple
silicon and Intel Macs on macOS 13 or later. It is ad-hoc signed, not Developer
ID signed or notarized.

**Windows / Linux:** follow the `portable setup guide <desktop/README.txt>`_.
The Python wheel and source distribution require Python and dependencies;
they are not standalone executables. Native packaging recipes are included.

The release workflow rebuilds the exact tagged source and attaches tested
macOS and Windows portable packages, CoreS3 firmware, source and checksums.
The Windows ZIP includes its executable and required runtime files.
The CoreS3 browser installer source is included; its
`hosted page <https://Hamspiced.github.io/Quadband_Card_Studio/>`_ requires the
one-time Pages setup and successful stable-release deployment described in
the `release guide <docs/RELEASING.txt>`_. Existing v1.02 downloads are unchanged.

Build and test
--------------

Native macOS requires Xcode command-line tools and Swift 5.9 or later:

.. code-block:: sh

   bash scripts/test.sh
   bash scripts/build-app.sh

The app and macOS ZIP are written to ``dist/``. Portable setup and test commands
are in ``desktop/README.txt``. Generate the full public source archive with:

.. code-block:: sh

   python3 scripts/source-archive.py

Local integration validation: **450 portable tests passed**, with two optional
hardware checks skipped; **172 native tests completed**, with five optional
hardware checks skipped and zero failures. The CoreS3 application/bootloader/partition artifacts
compile with pinned ESP-IDF dependencies; shared contract and sanitizer-backed
firmware host tests pass. These results do not certify physical writes,
BLE pairing, USB power/OTG interoperability or every target OS.
See `validation evidence and outstanding checks <docs/VALIDATION.txt>`_.

Credits
-------

Credit to **Tototo31 and Ashsh from the Dangerous Things community** for their
work on the **ACR122/ACR1552 scripts** and reader-configuration research that
informed these tools.

`Dangerous Things RF configuration discussion <https://forum.dangerousthings.com/t/acr1552u-change-rf-power-level-duty-cycle/28545>`_

The supplied reference script is not redistributed because its redistribution
permission has not been established. Reader controls use independent
implementations of documented commands. See `third-party notices <THIRD_PARTY.txt>`_.

License and documentation
-------------------------

Original application code is **GPL-3.0-only**; see `LICENSE <LICENSE>`_.
The Midwest Gadgets name and artwork have separate `branding terms <BRANDING.txt>`_.
Third-party dependencies retain their own licenses. No warranty is provided.

* `Detailed user guide and protocol references <docs/USER-GUIDE.txt>`_
* `CoreS3 firmware and desktop setup <docs/CORES3.rst>`_
* `CoreS3 bridge wire contract <protocol/cores3-bridge-v1.rst>`_
* `Windows/Linux setup <desktop/README.txt>`_
* `Release instructions <docs/RELEASING.txt>`_
* `Validation status <docs/VALIDATION.txt>`_
* `Password-free T5577 policy <docs/T5577.txt>`_
* `Third-party notices <THIRD_PARTY.txt>`_
* `Security policy <SECURITY.txt>`_
* `Contributing <CONTRIBUTING.txt>`_
