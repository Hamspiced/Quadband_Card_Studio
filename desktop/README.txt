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

A Python wheel/source tarball can also be installed with `python -m pip install path/to/quadband_card_studio-1.0.1-py3-none-any.whl`; pip resolves platform dependencies. These Python packages are **not standalone Windows/Linux executables**.

## Readers and workspaces

| Workspace | Supported paths |
| --- | --- |
| Magstripe Write / Read | MSR X6 HID or FF00 Bluetooth; MSR605 serial; MSR605X HID; MSR600 focused keyboard input, including configured sentinel-free field mappings |
| HF Workspace | ACS PC/SC, including ACR122U/ACR1552U; connected Chameleon Ultra; Classic 1K identity, automatic standard keys, 19 NDEF record types, data-block editing, `.qcard` files and Copy to infinity |
| UHF Workspace | Piswords UHF2/UCM60x serial, inventory/CSV, memory reads, verified User-memory writes and EPC changes; buzzer and RF power |
| LF Workspace | Chameleon Ultra USB or Nordic UART Bluetooth, firmware capability checks, LF scans, `.qlf` files; **password-free** T5577 writes on firmware exposing command 3016 |

Choose the serial device explicitly. Piswords usually uses 9600 baud, address 0; MSR605 usually uses 9600; Chameleon uses 115200. Close other reader utilities before connecting. The portable UI offers explicit baud selection; the native Mac UHF workspace also has automatic baud detection.

HF record writes require an already NFC-formatted **MIFARE Classic 1K**. This release does not format factory-blank cards or edit UID, keys or trailers. Other HF cards can be identified, not edited. A `.qcard` is a data-only 47-block profile, not a full credential/UID/key clone. A data copy does not configure NDEF sector keys. `.qcard` and `.qlf` use the same schemas as the Mac app.

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

Tests simulate readers; they do not write physical cards. The UI test uses Qt's offscreen backend. Protocol tests cover framing, partial responses, timeout/disconnect, NFC access bits, staged NDEF writes, protected blocks, batch continuation/removal, EPC selection/control bits, and password-free LF commands.

Optional **read-only** hardware tests are documented in `tests/test_hardware.py` and require explicit environment variables. Passing unit tests is not a claim of Windows/Linux hardware validation. See `../docs/VALIDATION.txt` for the current evidence.
