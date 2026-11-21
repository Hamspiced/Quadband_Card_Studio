# Quadband Card Studio — Windows & Linux

A local desktop app by Midwest Gadgets. This is the shared Python/Qt port; the native SwiftUI Mac app remains in `../Sources`. Source code: **GPL-3.0-only**. Branding: see `../BRANDING.txt`.

## Run from source

Use 64-bit Python 3.12. The build targets are Windows 10/11 x64 and Ubuntu 24.04 x64 (or a compatible newer Linux desktop). Other architectures/distributions can run from source if their dependencies are available, but are not release-tested.

Windows PowerShell, from the repository root:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python -m pip install -r desktop/requirements-build.txt
.\.venv\Scripts\python -m pip install --no-deps -e ./desktop
.\.venv\Scripts\quadband-card-studio
```

Linux, from the repository root:

```sh
sudo apt install python3-venv python3-dev build-essential swig libpcsclite-dev pcscd libccid libhidapi-hidraw0 libusb-1.0-0 bluez libegl1 libopengl0 libxcb-cursor0 libxkbcommon-x11-0
python3 -m venv .venv
.venv/bin/python -m pip install -r desktop/requirements-build.txt
.venv/bin/python -m pip install --no-deps -e ./desktop
.venv/bin/quadband-card-studio
```

A Python wheel/source tarball can also be installed with `python -m pip install path/to/quadband_card_studio-1.1.0-py3-none-any.whl`; pip resolves platform dependencies. These Python packages are **not standalone Windows/Linux executables**.

## Readers and workspaces

| Workspace | Supported paths |
| --- | --- |
| Magstripe Write / Read | MSR X6 HID or FF00 Bluetooth; MSR605 serial; MSR605X HID; MSR600 focused keyboard input, including configured sentinel-free field mappings |
| HF Workspace | ACS PC/SC, connected Chameleon Ultra, or CoreS3 Grove HF; Classic 1K/4K geometry and existing MAD/MAD2 NDEF; NTAG213/215/216 pages and NDEF on supported Grove HF; family-matched `.qcard` data and Copy to infinity |
| UHF Workspace | Piswords UHF2/UCM60x, JRD-4035 and YRM100 over USB serial; JRD-4035 over CoreS3; inventory/CSV, memory reads, verified User-memory writes and EPC changes; profile-specific RF power and Piswords-only buzzer |
| LF Workspace | Chameleon Ultra USB, Nordic UART Bluetooth, or CoreS3 USB host bridge; real firmware capability checks, LF scans, `.qlf` files; **password-free** T5577 writes on firmware exposing command 3016 |

Choose the serial device explicitly. Piswords usually uses 9600 baud, address 0; MSR605 usually uses 9600; Chameleon uses 115200. Close other reader utilities before connecting. The portable UI offers explicit baud selection; the native Mac UHF workspace also has automatic baud detection.

JRD-4035/YRM100: choose the matching Reader profile, use a USB-TTL adapter and select its port. Profiles default to 115200 baud, 8N1; older M100 firmware may need 9600. Match the module's UART logic voltage, cross TX/RX, share ground and provide suitable RF-module power. These profiles have no address byte or documented buzzer command. They support 16-bit word addresses, 64-word reads, 32-word writes and exact PC-plus-EPC memory selection for EPCs up to 28 bytes. Power controls are conservatively limited to 18–26 dBm; region/channel settings remain unchanged. Module tag reports include signed RSSI but not frequency. See the main README for wiring and limitations. Physical module operation remains unverified.

Normal HF record writes require a validated Classic MAD/MAD2 or supported NTAG21x NDEF layout. A recognized NTAG with valid CC/permissions and an entirely all-NULL user area can receive its first message. A separate, explicitly reviewed canonical formatter can initialize a fully preflighted factory-key/access Classic1K or exactly identified unlocked NTAG with an all-zero CC. It is not recovery for application cards, partial MAD layouts, nonzero OTP CC values, Classic4K, or protected tags. Generic writes still cannot change UID, keys, trailers, locks, CC, passwords/PACK or configuration; the canonical formatter alone has narrow fixed-output trailer/CC exceptions. Classic 4K retains its existing MAD2/data/profile support. NTAG total page counts are 45/135/231, with 36/126/222 user pages. Derive NDEF capacity from the actual validated CC/TLV layout, not physical memory size.

## Bulk NDEF encoding

In HF Workspace > Write records, choose the exact target family, add one to three records, and reorder or remove drafts as needed. Single-target editing keeps its existing larger record lists and Classic4K support. Bulk freezes the records, family and options; **Overwrite** and **Canonical format** default off. Existing different records or hidden nonzero data require explicit overwrite authorization. The selected byte limit is an upper bound: preserved control TLVs and actual target CC may reduce it.

Start bulk encoding, remove any initial card, then present one target. Read/preflight prepares a target-bound review; polling never writes automatically. Cancel is the default. Confirm that individual target once, wait for verified completion, remove it, and repeat. Authentication, unresponsive-reader and connection errors do not count as removal. A failed or uncertain mutation stops the job without automatic retry; read the affected target before deciding what to do next. Card contents and job state remain transient.

PC/SC NTAG identification requires exact supported GET_VERSION bytes, not just a selected dropdown or CC size. CoreS3 formatting additionally requires the optional `classic1k_format` or `ntag_initialize` capability under bridge protocol 1; older firmware remains unavailable for those actions. Chameleon formatting/NTAG is not supplied by a permissive raw-command fallback.

Legacy Classic 1K `.qcard` version 1 remains the original 47-block data-only format. New Classic 4K and NTAG profiles use `quadband.card.data` version 2 with explicit family and geometry. Files include every ordinary data unit, but no source UID, keys, password/PACK or protected metadata. Import and target preflight reject mismatched families and geometry. A data copy is not a full credential clone and does not configure NDEF keys or locks. `.qcard` and `.qlf` schemas are shared with the Mac app.

## CoreS3 Bluetooth hub

Use an M5Stack **CoreS3 ESP32-S3** with compatible bridge-v1 firmware: one RFID **or** RFID2 unit on Port A I2C, UHF-RFID/JRD-4035 on Port C UART, and a Chameleon Ultra attached to the CoreS3 USB host port with a data-capable OTG connection and appropriate power. The CoreS3 cannot simultaneously use its main USB port as a computer programming connection and a Chameleon host.

In the sidebar, open the bounded pairing window locally on CoreS3, choose **Scan CoreS3**, select the board, and connect. Confirm the displayed passkey on the board/system pairing prompt. Linux needs BlueZ and an active system pairing service. The client uses only the dedicated encrypted/authenticated bridge service, subscribes to indications before its fresh nonce-bound HELLO, and never falls back to the Chameleon/MSR UART service. The firmware must enforce bonded, authenticated access; portable host APIs cannot independently inspect MITM protection. Only one desktop application can own the bridge connection at a time.

Select **CoreS3 · Grove HF** explicitly in HF Workspace. For UHF or USB Chameleon, select **CoreS3** in that workspace's connection mode, refresh the detected endpoint, and connect; all endpoints share the one BLE session. Missing peripherals or capabilities remain unavailable. Generation changes invalidate selections and write previews rather than silently switching to replacement hardware. Existing direct USB, PC/SC, serial and Bluetooth paths remain available.

NTAG operations initially require a detected Grove HF driver advertising `ntag21x`; USB Chameleon raw ISO14443 commands and NTAG writing are not exposed. Optional supplied password/PACK fields are enabled only with `ntag_auth`. Credentials are transient, are never guessed or saved in profiles/settings, and password/PACK pages are not read into snapshots. Known external Lock Control TLV mappings are preserved; unknown or payload-overlapping reserved-memory layouts remain read-only. Writes require readable permission metadata, validated CC, unlocked user pages and any explicit authentication required by the target. Both Classic and NTAG NDEF writes clear the message length first and commit it last after data verification.

The session permits one pending physical operation, binds responses to session/request/generation, bounds framing/queues and fails closed on cancellation, timeout or protocol loss. It does not replay writes. A lost link after transmission leaves an explicitly unverified outcome; reconnect and read before deciding whether to retry. Chameleon HF/LF operations hold one exclusive lease and normal release restores its original reader/emulation mode.

CoreS3 and expanded-family validation is currently simulated/offscreen only. Pairing behavior, board power/OTG, each actual Grove reader, Chameleon firmware, and physical card reads/writes still require hardware acceptance testing. No firmware flash or physical card write is part of the automated tests.

Copy to infinity accepts a scan or a file, waits for source removal, and lists successes/failures while continuing after card failures. A held card is not retried. Loss of the reader requires another observed removal after reconnection. Stop lets the current card finish. Other operations remain disabled while hardware work is in progress.

The LF writer never sends legacy password-setting commands, never writes block 7, and never enables PWD. EM410x, Electra, Viking, ioProx, PAC/Stanley and Jablotron have open-write encoders. HID Prox and Pyramid remain readable/saveable but cannot be cloned by this password-free implementation. Older firmware without 3016 can read but cannot write LF cards. See [T5577 policy](../docs/T5577.txt).

## Driver setup

Windows: use the reader manufacturer's signed CCID/serial driver if the OS does not recognize it. Keep the **Smart Card** service available. Do not replace the ACS CCID driver with WinUSB: PC/SC needs the CCID interface. Some ACR1552 drivers require enabling documented escape-command support before reader settings work. A denied control command is reported rather than silently changing driver settings.

Linux: enable the pcscd service/socket if your distribution does not start it automatically. Add your user to the distribution's serial-device group (`dialout` on Ubuntu, sometimes `uucp`), then sign out/in. The optional `packaging/70-quadband.rules` grants active local-session access only to the known MSR/Chameleon/Piswords interfaces. Review it and install it into `/etc/udev/rules.d/`, reload udev rules and reconnect readers. Never run the whole GUI as root. The ACR122 kernel NFC driver can conflict with PC/SC on some distributions; consult your distribution/ACS documentation rather than disabling unrelated devices globally.

Bluetooth: Windows/Linux use Bleak's system pairing flow for Chameleon and an authenticated battery read where present. Enter your configured device PIN if prompted. Linux requires BlueZ and an active system pairing service. Wake the reader and close phone/desktop applications that already hold its connection. The app does not change pairing settings or clear bonds. Pairing behavior remains firmware/OS dependent; the native Mac pairing issue is documented in the main README.

## Build standalone packages

To build the Windows ZIP from an unpushed Mac/Linux checkout, use the [local Windows build guide](../docs/WINDOWS-BUILD.txt). This produces a Windows x64 executable with its private runtime; it does not require a source upload. Native Windows startup/hardware validation is still required.

On the **target OS**, after installing the dependencies above:

```sh
python desktop/packaging/build.py
```

Use your venv's Python. Windows creates a zip containing `QuadbandCardStudio.exe` plus its runtime folder. Linux creates a tar.gz containing `QuadbandCardStudio` plus its runtime folder. Keep the entire extracted folder together; these are portable directory packages, not installers. They include licenses, dependency metadata, setup instructions, udev rules (Linux), and an archive of the application source. Binaries are unsigned; public signing can be added later.

The GitHub Actions workflow builds and tests on native Windows and Linux runners and uploads downloadable artifacts. It does not publish a public release automatically. Builds from this Mac validate common code and UI only, not target-OS binaries or reader drivers.

## Tests

```sh
python -m pytest desktop/tests
python -m ruff check desktop/src desktop/tests
```

Tests simulate readers; they do not scan real Bluetooth devices or write physical cards. UI tests use isolated temporary settings and Qt's offscreen backend. CoreS3 tests read the shared `protocol/` manifest and golden fixtures. In an isolated implementation worktree, an untracked copy may instead be placed at `.build/cores3-tests/protocol/`. Tests cover fragmented/coalesced framing, nonce binding, generation changes, timeout/cancellation/disconnect, raw reader routing and allowlists, Classic 4K access/MAD2, NTAG family/locks/auth/CC/TLVs, length-last NDEF, exact profile schemas, batch removal gates, and UI operations through the simulated bridge.

Optional **read-only** hardware tests are documented in `tests/test_hardware.py` and require explicit environment variables. Passing unit tests is not a claim of Windows/Linux hardware validation. See `../docs/VALIDATION.txt` for the current evidence.
