# Quadband Card Studio by Midwest Gadgets

**Version 1.0.2 — macOS, Windows and Linux source. GPL-3.0-only.**

- **macOS:** native SwiftUI application; build with `bash scripts/build-app.sh`.
- **Windows / Linux:** shared desktop application in [desktop/](desktop/README.txt), with USB HID, serial, PC/SC and Bluetooth adapters.
- **Publishable source:** `python3 scripts/source-archive.py` creates a source archive with all original application code, tests, resources, build scripts and CI. See [release instructions](docs/RELEASING.txt), [validation](docs/VALIDATION.txt), [third-party notices](THIRD_PARTY.txt) and [branding terms](BRANDING.txt).
- **T5577:** password writes are disabled in every version. Open block writing requires compatible firmware; see [password-free policy](docs/T5577.txt). No password controls remain in the UI.

## v1.02 updates

- Added M5Stack JRD-4035 and YRM100 profiles for USB-to-TTL serial adapters, defaulting to 115200 baud.
- Added M100-family inventory with signed RSSI, memory reads, verified EPC/User-memory writes and RF-power read-back.
- Preserved Piswords support with separate reader protocols and profile-specific limits and controls.
- Added manufacturer-packet, simulated serial, selection-safety and UI regressions. Physical JRD-4035/YRM100 operation remains unverified; see the setup and limits below.

## V1.01 updates

- Choose System, Light or Dark under Appearance in the sidebar; the choice is saved across launches.
- The sidebar logo is larger and uses high-quality, display-density-aware rendering.
- UHF Workspace is the first contactless option, followed by HF and LF.
- Chameleon Ultra USB/Bluetooth connection controls are below the magstripe connection in the sidebar and share one connection across HF and LF workspaces.

The Windows/Linux implementations are source ports with native packaging jobs, not a claim that physical readers or target-OS binaries have been validated from this Mac. The sections below describe the native Mac workflows; the portable guide documents its setup and differences.

## Credits

Credit to **Tototo31 and Ashsh from the Dangerous Things community** for their work on the ACR122/ACR1552 scripts and reader-configuration research that informed these tools. RF preset references: https://forum.dangerousthings.com/t/acr1552u-change-rf-power-level-duty-cycle/28545 . The supplied reference script is not redistributed; the application uses independent implementations of documented ACS commands. See THIRD_PARTY.txt for attribution and licensing details.

Native macOS app for MSR X6 magnetic-stripe writing over USB or Bluetooth, MSR605 serial read/write, MSR600 keyboard reading, MIFARE Classic 1K NFC records with ACS PC/SC readers or Chameleon Ultra, Piswords UHF2, M5Stack JRD-4035 and YRM100 UHF tag scanning and memory/EPC editing, and Chameleon Ultra LF reading/T5577 writing over USB or Bluetooth. Includes the supplied Midwest Gadgets logo. The sidebar groups **Magstripe Write** and **Magstripe Read**, with **HF Workspace** for NFC and **UHF Workspace** for UHF tags, and **LF Workspace** for LF cards; shared Chameleon connection controls are in the sidebar. SwiftUI, IOKit, CoreBluetooth and the system PC/SC framework; no third-party runtime dependencies.

## Open the app

Open **dist/Quadband Card Studio.app**. The universal app supports Apple silicon and Intel Macs running macOS 13 or later. You can drag it into Applications. It is locally ad-hoc signed, not Developer ID signed or notarized for public distribution. The internal bundle identifier remains unchanged to preserve the existing app identity.

## MSR X6: USB and Bluetooth

1. Close EasyMSR and other reader utilities. In the sidebar, choose **USB** or **Bluetooth**.
2. USB: attach a data cable and **Refresh**. Bluetooth: turn on the X6, **Scan**, allow macOS Bluetooth access, and choose the reader. Click **Connect**. Writing stays disabled until the communication handshake succeeds.
3. Select **HiCo** or **LoCo** for the card. Standard ISO densities are **210 / 75 / 210 BPI**. Bits per character are **7 / 5 / 5**; the device supplies parity and LRC. HiCo is independent of density.
4. Enter payloads or include outer `%…?`, `;…?`, or Track 3 `+…?` markers. Unsupported characters and oversized tracks are rejected before writing.
5. Click **Write & verify**. Wait for the prompt and swipe a blank test card. After the write acknowledgement, wait for the second prompt and swipe the **same card again**. Every requested track is compared. Empty, unrequested tracks are labeled “Not requested” and are not certified by the comparison.

Writing may replace data on all tracks; firmware varies in its treatment of empty fields. Cancel resets/closes the writer connection; reconnect for the next operation. A timeout after writing is reported as an incomplete verification, never as a verified write.

**Short diagnostic data** fits custom all-75-BPI testing. **Midwest sample** uses the supplied test data and selects standard densities. Track 1 at 75 BPI conservatively allows 25 payload characters; numeric tracks allow 37. The original Track 1 and Track 3 data do not fit at 75 BPI.

Bluetooth support targets the observed X6 BLE service FF00, FF02 writes and FF01 notifications, carrying the serial MSR commands directly. It does not apply USB packet framing to BLE. Other X6 firmware, Bluetooth Classic/RFCOMM-only variants, and unrelated Bluetooth readers are not automatically compatible.

## MSR605: USB serial

Choose **Serial** in the magstripe connection panel, select the MSR605's serial port and choose its baud rate (normally **9600**, 8N1, no flow control). Click **Connect**; the existing magstripe read, write and second-swipe verification workflows are enabled only after an MSR handshake. MSR605X models that expose USB HID use the **USB** connection instead. A USB-to-serial driver may be required by older adapters. The app does not probe every serial device as a magstripe writer: select the correct port, especially when the Piswords reader is attached too.

Serial communication has been tested against a simulated MSR605 on a macOS pseudo terminal, including fragmented replies, timeouts and cancellation. A physical MSR605 was unavailable for validation.

## UHF Workspace: Piswords UHF2

1. Attach the reader, open **UHF Workspace**, select its USB serial port, and **Connect**. Automatic baud detection checks firmware at supported rates; the supplied reader uses **9600 baud**, address **0**, and identifies as **UCM601 firmware 2.3**. The manufacturer's app defaults to 115200. You can set the baud and address explicitly before connecting.
2. Click **Scan tags**. The list groups reports by EPC and shows antenna, frequency, signal and read count. **Stop scanning** before accessing tag memory. **Save scan** exports the current list as CSV.
3. Select a tag. **Tag memory** reads EPC, TID, User or Reserved memory. Starting word and length are decimal; one word is two bytes. Enter the tag's eight-hex-digit access password, or leave the factory `00000000` value. A protected or missing bank returns an error rather than fabricated data.
4. **Edit EPC:** enter a new hexadecimal EPC, then **Review EPC change → Write & verify**. The current EPC is used as an exact access filter; the app reads the filter back before proceeding, checks the tag again, preserves the other protocol-control bits, updates the EPC length, sends one write, selects the new EPC, and reads the bytes back. EPCs must contain 2–62 bytes in complete words, within the tag's actual capacity. Keep only the intended tag near the reader; tags with identical EPCs cannot be distinguished by this filter.
5. **Write User memory** updates only the requested User-bank range and verifies it. Reserved/password and TID writes, lock/kill commands, firmware flashing, and region changes are not exposed. **Reader tools** includes buzzer on/off and RF power with a read-back check.

Write commands are never automatically retried. A timeout or failed read-back means the tag may be partially changed; scan/read it before another attempt. Reader success alone is not shown as a verified tag write. The workspace handles the supplied UHF2/UCM60x protocol over USB serial; unrelated Piswords products, TCP/IP and HID UHF transports are not claimed as supported. UHF EPC Gen2 tags are distinct from HF NFC/MIFARE cards.

Native live validation read the connected UCM601 version, antenna, power and region, scanned the supplied test tag, and read its TID and EPC/PC words through the production transport. Packet checksums, fragmented/interleaved reports and the manufacturer's memory layouts are covered by tests. EPC writes and verification are tested with a simulated tag; **the physical test tag has not been changed**.

## UHF Workspace: M5Stack JRD-4035 and YRM100

Choose **M5Stack JRD-4035** or **YRM100** under Reader profile, select the **USB-to-serial adapter's port**, then connect. These profiles use the M100-family binary UART protocol, not the Piswords protocol, keyboard output or an AT/debug console.

### USB-TTL connection

- Use a USB-to-TTL UART adapter with logic voltage matched to the specific module; an RS-232-level adapter is not interchangeable.
- Cross adapter TX to module RX and adapter RX to module TX, with a common ground. Supply the module's specified power from a suitable source; do not assume the adapter's low-current logic-voltage pin can power its RF stage.
- For the M5Stack U107 Grove connector, the documented module pins are black GND, red 5V, yellow UART_RX and white UART_TX. Power voltage and UART logic voltage are separate requirements. Bare YRM100 boards and carrier variants need their own pinout and electrical specifications checked.
- The default profile baud is **115200**, **8 data bits, no parity, 1 stop bit**, with no flow control. Older M100 firmware may use **9600**; select it explicitly, or use native macOS automatic baud detection (115200, 9600, 19200).
- A module connected behind an M5 controller's ordinary USB/debug port is not automatically a serial reader. That arrangement needs a transparent binary UART bridge; the direct USB-TTL connection does not need controller firmware.

### Supported operations and limits

Inventory reports EPC, PC and signed RSSI. Repeated single-poll commands provide a continuous scan until Stop; valid no-tag replies are not connection failures. The module's tag notifications do not contain a channel frequency, so the UI says **Frequency not reported** and the CSV frequency field stays empty.

After scanning and selecting a tag, read EPC, TID, User or Reserved memory, or review an EPC/User-memory write. The application selects the complete **PC word plus EPC**, checks the selection read-back and returned identity, and verifies write results against the tag's memory. This avoids matching a longer EPC that only shares a prefix. Identical PC/EPC tags still cannot be distinguished, so keep one intended tag in range.

Module limits: **16-bit word addresses**, **1–64 words per read**, and **1–32 words (64 bytes) per write**. The one-byte select-mask length limits exact memory access and EPC changes to **2–28-byte EPCs**. Longer EPCs can be inventoried but are not eligible for memory operations in this implementation. EPC changes preserve the other PC bits and leave the CRC word to the tag.

RF-power controls use a conservative **18–26 dBm** range and verify the module's hundredths-of-dBm value by reading it back. Region, channel and frequency-hopping settings are not changed. There is no documented module buzzer command, so those buttons are unavailable. Password/Reserved and TID writes, lock/kill operations, firmware updates and changes to the module's stored baud rate are not exposed.

The drivers are independent implementations of the [M5Stack protocol manual](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/842/Unit-RFID-UHF-Protocol-EN.pdf) and M100 firmware command reference. Protocol fixtures and simulated serial readers validate framing, error handling, selection and verified writes. **Physical JRD-4035/YRM100 modules have not been tested with this application.**

## MSR600: swipes with or without markers

Use the MSR600's USB keyboard mode. Choose **Magstripe Read → MSR600 · Keyboard**, click **Start reading**, and swipe with this app active. No global keyboard hook or Accessibility permission is used.

- **Automatic · track markers** understands `%track1?;track2?+track3?`, including a second semicolon for Track 3 and Enter/Tab separators.
- For readers omitting the markers, select **Input format** to assign one, two or three fields to their tracks. Multiple fields must be separated by Enter or Tab. A CRLF is one separator and empty fields are preserved.
- The **Raw swipe** remains available when automatic interpretation fails. Changing Input format reprocesses it without another swipe. Arbitrarily concatenated fields with no markers or separators cannot be split reliably.
- Use a US keyboard input source if punctuation is altered. Capture finishes after a short idle gap. **Paste swipe** uses the same parser and reads the clipboard only when clicked.

The X6 itself can also read via USB or Bluetooth. **Verify existing card** compares a read with the editors without writing.

## HF Workspace: ACR122U, ACR1552U and compatible ACS readers

HF Workspace uses macOS PC/SC and the ACS storage-card APDUs shared by ACR122U and compatible readers such as ACR1252U and ACR1552U. Readers refresh when the workspace opens; the app prefers a contactless PICC/CL interface and excludes SAM slots. A reader must appear in **Refresh readers**; install its manufacturer's macOS CCID driver if macOS does not recognize it. USB/PC/SC NFC is supported; proprietary reader Bluetooth connections are not included.

The implemented memory profile is **MIFARE Classic 1K**, including Gen1a Magic cards in normal authenticated Classic mode. A recognized Classic 1K PC/SC ATR is required before memory editing. **Identify card** can display the UID and ATR of other cards when their reader supports the UID command; it does not enable unsupported memory operations.

### Read and write records

1. Place one card on the reader and click **Read card**. The **Read** tab shows card details, record usage/capacity, writability and decoded records. The app uses the public MAD key for sector 0, the public NDEF key for NFC sectors, and the factory key as a fallback. A custom key from Advanced is also supported. Failed authentication is followed by a PC/SC reset and card identity check before another attempt.
2. Select **Write records → Add record** and choose a type below. Build a queue, edit or reorder records, and see its encoded size before writing. **Use existing** loads the card's current records into the queue so new records can be added to them.
3. Click **Write records & verify**. Writing replaces the NDEF message with the queue. The app checks the card identity, original message and access bits; publishes a zero message length while updating; commits the real length last; and reads the message back for comparison. UID, keys, directory and sector trailers remain unchanged.

Record writing currently requires an **already NFC-formatted MIFARE Classic 1K card**, such as the URL card from NFC Tools. Formatting a factory-blank card would require changing its directory and sector keys and is not included. The app does not automatically reformat a card. A full-card NDEF allocation has 716 message bytes; other directory allocations may have less. Unsupported chunked messages are reported, not silently rewritten.

| Record type | Stored format |
| --- | --- |
| Text | NFC Text RTD with language code |
| URL / URI, Custom URL / URI | NFC URI RTD, with standard prefix compression |
| Unit.Link | URI for an existing Unit.Link profile |
| Search | Google, DuckDuckGo or Bing search URI |
| Payment Link, Social Networks, Video Link, File Link | URI supplied by the user |
| Application | Android Application Record (`android.com:pkg`) |
| Mail | `mailto:` URI with optional subject and message |
| Contact | vCard 3.0 MIME record |
| Phone | `tel:` URI |
| Location | `geo:` URI with validated latitude/longitude |
| SOS | Text record containing emergency information |
| Bluetooth | Bluetooth Classic OOB MIME record with address and optional name |
| 3D Printing | OpenSpool 1.0 JSON MIME record |
| Wi-Fi | WSC MIME record for WPA2 Personal or an open network |
| Data | Custom MIME type with hexadecimal or UTF-8 payload |

The receiving phone or application decides how to handle a record. OpenSpool readers must also support the physical card type; many use NTAG tags. Wi-Fi passwords are stored in the record on the card. This app displays record contents without automatically opening links. Unknown records can be retained in the queue; unsupported record types cannot be edited into a different type accidentally.

### Advanced memory editing

**Advanced** contains automatic/custom key settings, sector selection, card identification and the block editor. Choose a sector, select a data block, and edit exactly **16 bytes** in hexadecimal. **Write block & verify** checks the UID/ATR and original bytes before writing, then reads the block back. With automatic keys enabled it uses the credential that read that sector. Disable automatic keys to use only the entered Key A or Key B. Access failures are shown per block; unread bytes are not filled with invented values.

The block editor writes the 47 ordinary data blocks. **Block 0 and sector trailers are read-only.** Gen1a backdoor/UID changes, key or access-bit changes, Classic 4K, DESFire, NTAG memory access, and raw memory-dump import/export are not implemented. Sector-trailer key bytes returned by ordinary reads are masked or restricted by the card and are not a complete key backup. Gen1a capability cannot be established from an ordinary Classic ATR alone.

## HF reader tools: ACR122U and ACR1552U

Under **HF Workspace → Tools → Reader tools**, select **Read reader settings**. Some Mac drivers require a card on the ACR122U for reader controls; the app uses its documented pseudo-APDU path when a card is present. ACR1552U escape controls require a compatible ACS macOS driver.

- **Buzzer:** stage on/off or choose individual supported events, then **Review & apply**. ACR122U controls its card-detection beep; it cannot report the current buzzer state, so the result is labeled acknowledged rather than read-back verified. ACR1552U controls card-presence/removal beeps and, on supported firmware, power-up beeps while preserving LED bits.
- **ACR1552U RF presets:** Tototo31 (80% power, 250 ms RF-off interval), Ashsh (100%, continuous field), or hardware-default power with 250 ms. These stage the relevant reader settings from the user-supplied `acr1552u_rf_config.py`; review the exact changes and apply them. RF power restarts the reader and is verified without resending the write. Percentage controls may have no effect on some hardware and do not measure actual field strength.
- **Advanced reader controls:** polling, card technologies, maximum speed, supported LEDs, UID keyboard output, keyboard language, USB mode and suspend. Unsupported firmware features are disabled. Shared register bits are preserved. The app validates the selected reader/firmware/serial and checks for changed settings before sending edits. Some settings require unplugging and reconnecting; this is shown in the review/results. Keyboard-only mode removes the PC/SC interface and requires an external ACS utility to restore it.
- **ACR122U equivalents:** polling interval, enabled card types, automatic ATS, buzzer and runtime RF on/off. Its documented commands do not provide an ACR1552U-style percentage-power setting. Automatic polling may reactivate a field turned off manually.

The native controls avoid a Python installation. RF preset credit: **Ashsh and Tototo31**, [Dangerous Things discussion](https://forum.dangerousthings.com/t/acr1552u-change-rf-power-level-duty-cycle/28545). The supplied reference script has no established redistribution license and is excluded from public packages and default builds; its local original remains available in the original working folder. Independent reader controls remain built in. [Flowtter's py-acr122u](https://github.com/Flowtter/py-acr122u) provides a similar ACR122U utility; Quadband implements documented ACS commands independently.

The connected ACR122U reported firmware **ACR122U214** and its polling settings were read successfully via the card-present pseudo-APDU path. No persistent reader setting was changed during validation. ACR1552U was unavailable for physical tests. Reader-setting tests cover model/firmware checks, shared bits, stale snapshots, incompatible USB options, malformed responses, partial failure and restart verification. Hardware support does not expand the app's implemented card-memory profiles beyond those described above.

## Saved credentials, Write from file and Copy to infinity

These features are under **HF Workspace → Tools**. A **.qcard credentials file** is a versioned JSON profile containing exactly the 47 ordinary Classic 1K data blocks (752 bytes). It contains no source UID, authentication keys, manufacturer block or sector trailers. Imported files are size-limited and validated for format, version, complete block coverage, duplicates, prohibited blocks and byte lengths before any write.

- **Save credentials:** read all sectors successfully, then select Save credentials. Choose a local filename. A partial scan cannot be saved as a complete profile.
- **Write from file:** choose a `.qcard` file. The app shows its profile and data scope. Place a target card on the reader, select its authentication key, and click **Write profile & verify**.
- **Copy to infinity:** select the feature, then **Scan card** or **Upload card credentials**. The scan reads all sectors using automatic keys or the custom-key setting. Enter the target cards' key in Advanced if it differs, then **Start copying**. Remove the source/current card first; present target cards one at a time.

Each target is authenticated and all data blocks are checked for readability before modification. Only ordinary data blocks are updated, and all 47 must match on read-back for success. Already matching blocks are read and verified without unnecessary rewrites. UID and sector keys remain unchanged.

A data-block profile is not an NDEF formatter. Copying NDEF data onto a factory-blank card while preserving its factory keys and trailer settings does not make that target a phone-readable NFC tag. Use matching, already-formatted cards for that workflow, or the record writer for an existing NFC-formatted card.

A failed target creates a failure row and does **not** end the batch. Remove it and present the next card. The app waits for actual card removal between attempts, so even cards sharing a factory UID are processed correctly and a held card is never retried in a loop. Reader disconnection pauses the batch until reconnection and a fresh removal/presentation cycle. **Stop batch** stops after the current reader command; an interrupted copy can leave a partially changed card, which is reported as failed. Keep the app open while copying.

The results panel shows success/failure details for the latest 500 cards plus running totals. Results are held in memory. Each batch uses the selected Key A or Key B for all sectors on its targets; per-sector key sets and key recovery are not implemented. Credentials are uploaded only into the local app, never to a server.

## Privacy and session state

No card data or authentication keys are uploaded or automatically saved. Save credentials writes data to disk only when explicitly selected. Magnetic-stripe activity messages omit track contents. Explicit **Copy** puts read results on the macOS clipboard. **Clear session** clears the app's editors, raw swipe, NFC/UHF results and entered keys/passwords; it does not clear the system clipboard. Keep the app open during a card write. Leaving a UHF scan running continues to collect tags; stop it before closing the app.

## Build and validation

Requires Xcode command-line tools and Swift 5.9 or later:

```sh
bash scripts/test.sh
bash scripts/build-app.sh
```

The build produces a universal `.app` and `dist/Quadband-Card-Studio-macOS.zip`. Open `Package.swift` in Xcode to edit/debug. Build caches stay in `.build`. The PC/SC C bridge keeps Apple's legacy C API behind a Swift-compatible boundary. To regenerate the icon from `Resources/MidwestGadgets.png`, run `swift -module-cache-path .build/ModuleCache scripts/make-icon.swift`, then rebuild.

Automated checks cover MSR command examples and validation, USB fragmentation, BLE response fragmentation, malformed packets, sentinel-free swipe mappings, focused keyboard capture, and simulated NFC reads/writes. NFC cases include key failures, reserved-block protection, card replacement, stale content and failed read-back verification. Additional checks validate credentials files, verify UID/trailer preservation, prevent writes when preflight authentication fails, report partial copies, and confirm the batch continues after a card failure while requiring removal. NDEF checks cover all 19 record builders, reference URI bytes, malformed and long messages, automatic-key fallback with a halted-card simulation, directory/access validation, staged writes, capacity limits and preservation of other TLVs. Screen-render checks exercise the native SwiftUI views and record editors.

A non-writing communication handshake succeeded with the user's physical MSR X6 over FF00/FF02/FF01. The production PC/SC code read all 64 blocks of the user's NFC-formatted Classic 1K card through the ACR122U with no errors and decoded `https://midwestgadgets.org`: one writable record, 23/716 bytes, matching the supplied NFC Tools screenshot. That live scan also passed 47-block credentials encoding/decoding validation in memory, and the reader's card-presence API reported the card correctly. No card was written and no credentials file was saved by the check. **Physical magnetic-stripe writes and NFC card writes still need testing with the user's cards and reader.** Simulated tests do not establish hardware compatibility across all firmware variants.

Opt-in hardware checks are available by running tests with `QUADBAND_UHF_TEST_PORT=/dev/cu.usbmodem…` (the actual selected port) and/or `QUADBAND_ACR_TEST=1`. They scan/read only; they do not write tag data or change persistent reader settings. A UHF access filter is temporarily set to read the selected tag and then cleared. The ACR122U check needs a card present on drivers without direct-mode support.


## Chameleon Ultra · LF and HF (v2.0 password-free writes)

The **Chameleon Ultra sidebar panel** connects a Chameleon Ultra over USB (115200 baud) or Bluetooth (Nordic UART). Choose the device and connect; Bluetooth pairing, when enabled on the Chameleon, is handled by macOS using your configured PIN. Close competing Chameleon apps before connecting. In v1.6.1 the app first reads the firmware’s protected Bluetooth battery characteristic before subscribing to its UART, allowing macOS to initiate pairing through an authenticated GATT read. It allows up to 90 seconds for setup/PIN entry, supports firmware without that battery service, and never disables pairing or clears bonds automatically. If the device battery is low, keep USB connected for power. Official 2.x firmware is checked at connection time, and the reported command capabilities determine available formats. Chameleon Lite lacks reader hardware and is rejected.

- **Scan & read:** automatically tries the available LF formats, or scans a selected type. EM410x, HID Prox and Viking are available on the connected 2.1 firmware. Newer firmware may additionally expose Electra, ioProx, PAC/Stanley, Jablotron and Pyramid; Pyramid is read-only. HID has a format hint selector when automatic detection is ambiguous. Reading captures the transmitted identity, not a full T5577 memory dump.
- **Clone card:** scan a source (or open a `.qlf` file), remove it, place one writable T5577 target, and review the write. Source/target selection is physical; leave only the target at the antenna.
- **Write default:** writes the editable EM410x test ID, initially `0102030405`. This is a working test configuration, not a chip factory erase.
- **Recover T5577:** attempts to restore the same readable EM410x configuration and test ID using open block writes, even when the target cannot currently be scanned. It replaces data rather than retrieving lost data. Password-protected targets, unsupported downlink modes, permanent locks, and physical damage prevent this workflow from recovering a card. This is not a universal unbrick command.
- **No T5577 passwords:** the app rejects legacy firmware commands that set passwords. Only command 3016 open writes are used, with no authentication, no password-block writes and the PWD bit off. Firmware without this command cannot write LF tags. Password-free encoders cover EM410x, Electra, Viking, ioProx, PAC and Jablotron. HID Prox and Pyramid remain scan/save only. Password-protected targets cannot be recovered by this path. LF files contain format/ID only. See [the implementation policy](docs/T5577.txt).
- **Verification:** an acknowledgement alone never counts as success. The app scans again and compares the identity/format. Timeout or malformed responses close the connection; writes are not automatically retried. Cancel cannot undo an already transmitted write. Normal Disconnect restores the device mode observed at connection; abrupt unplug/quit may leave reader mode active.

To use the same connection for **HF**, connect in the Chameleon Ultra sidebar panel, open **HF Workspace**, refresh readers, and select **Chameleon Ultra · USB** or **Chameleon Ultra · Bluetooth**. The existing NFC record editor, Advanced data-block editor, `.qcard` files, and Copy to infinity work with **MIFARE Classic 1K**, including Gen1a in normal authenticated mode. Only data blocks are written; UID/manufacturer blocks, sector trailers, access bits and keys remain unchanged. NDEF writes require an already formatted Classic 1K card. Other ISO14443-A cards can be identified; their memory formats are not supported by this editor. ACS configuration controls are shown only for ACS readers.

HF and LF operations share an exclusive connection, including during bulk-copy waits. HF writes check identity, stale data, keys and access conditions, then read back the result. NFC record writes publish the message length last. `.qlf` identity files and `.qcard` HF data files are deliberately different formats.

Protocol sources: [official command definitions](https://github.com/RfidResearchGroup/ChameleonUltra/blob/main/software/script/chameleon_cmd.py), [firmware command handlers](https://github.com/RfidResearchGroup/ChameleonUltra/blob/main/firmware/application/src/app_cmd.c), [T5577 writer](https://github.com/RfidResearchGroup/ChameleonUltra/blob/main/firmware/application/src/rfid/reader/lf/lf_reader_main.c), [T5577 configuration](https://github.com/RfidResearchGroup/ChameleonUltra/blob/main/firmware/application/src/rfid/nfctag/lf/protocols/t55xx.h), and [Bluetooth UART service](https://github.com/RfidResearchGroup/ChameleonUltra/blob/main/firmware/nrf52_sdk/components/ble/ble_services/ble_nus/ble_nus.c). The exact installed revision `97dfe5b` was also checked. No firmware or emulation slots are altered by the app.

Validation for v1.6: 84 automated tests passed; five physical-hardware tests are opt-in and skipped during the ordinary run. USB identification and LF scan commands passed on the connected Chameleon (`v2.1.0-10-g97dfe5b`), but no LF tag was detected. USB HF identification recognized a Classic 1K card; subsequent reads returned no-card responses, so a complete physical HF read is not yet verified. Bluetooth discovery and service connection reached the device, which rejected notifications with insufficient encryption pending macOS pairing. No physical LF/HF write, recovery, password change, or firmware update was performed.

Opt-in read-only checks use `QUADBAND_LF_TEST_PORT`, `QUADBAND_HF_CHAMELEON_PORT`, or `QUADBAND_LF_BLE_TEST=1` with the matching `ChameleonHardwareTests` test filter. Physical writes/recovery need separate testing on intended expendable targets; software fixtures verify write layout, protected-block preservation, failed verification, interrupted record writes, and no automatic write retries.

## Protocol references

- [MSR605 Programmer's Manual](https://freedelivery.com.ua/files/MSR605_Programmers_Manual.pdf): MSR command values and response/data framing.
- [DEFTUN X6 datasheet](https://www.hitools-access.com/upload/MSR-X6-BT-datasheet.pdf): card formats and density/coercivity settings.
- [MSR600 manufacturer manual](https://pdt.static.globalsources.com/IMAGES/PDT/SPEC/203/K1171742203.pdf): keyboard-mode packets.
- [MSR605X protocol investigation](https://github.com/Guiferreira2000/MSR605x_GUI): USB framing reference; implementation here is independent.
- [ACS ACR122U API manual](https://www.acs.com.hk/download-manual/419/API-ACR122U-2.03.pdf): PC/SC ATR, UID, key loading, authentication and block commands. The downloaded document identifies itself as version 2.04.
- [ACS ACR1252U API manual](https://www.acs.com.hk/download-manual/6402/API-ACR1252U-1.17.pdf): related ACS PC/SC reader command family.
- [NXP AN1305](https://www.nxp.com/docs/en/application-note/AN1305.pdf): NDEF storage on MIFARE Classic, standard keys and access conditions.
- [NXP AN10787](https://www.nxp.com/docs/en/application-note/AN10787.pdf): MIFARE Application Directory and CRC.
- [Android NdefRecord](https://developer.android.com/reference/android/nfc/NdefRecord): NDEF record formats and application records.
- [AOSP Bluetooth handover](https://android.googlesource.com/platform/packages/apps/Nfc/+/215625430550bc87dc367937189b8e5285a346e7/src/com/android/nfc/handover/HandoverManager.java): Bluetooth OOB MIME payload layout.
- [WPS attribute definitions](https://w1.fi/wpa_supplicant/devel/wps__defs_8h_source.html): WSC attributes and authentication/encryption values.
- [OpenSpool RFID specification](https://openspool.io/rfid.html): filament JSON schema.

- [ACS ACR1552U reference manual 1.09](https://www.acs.com.hk/download-manual/13473/REF-ACR1552U-Series-1.09.pdf): escape controls, firmware requirements and card-memory APDUs.
- User-supplied `PiswordsUHF2/PISWORDSreader_win64.exe`: embedded `com/xyang/rfidreader/CommandContent.java`, `Commands.java`, `rfidClient.java`, `myRfidCallback.java` and `FrameMainUI.java`. Used to independently implement framing, command layouts, acknowledgements, RSSI interpretation and EPC/memory operations. The vendor executable/runtime is not redistributed with the Mac app.
