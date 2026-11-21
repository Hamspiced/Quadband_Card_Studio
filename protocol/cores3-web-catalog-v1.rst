CoreS3 browser-install catalog, version 1
========================================

``cores3-web-catalog-v1.json`` defines the catalog shared by release export,
Pages staging and the browser installer. It is separate from BLE bridge
framing and does not authorize remote reader operations.

Release identity
----------------

``schemaVersion`` is 1. The supported board/chip is M5Stack CoreS3 /
ESP32-S3, with 16,777,216 flash bytes and DIO/80 MHz programming settings.
Application version and firmware version are independent numeric semantic
versions. ``releaseTag`` identifies the desktop release; historical v1.01
and v1.02 retain their explicit application-version mapping.

``sourceCommit`` identifies the checked-out source commit. ``sourceDirty``
must be a boolean: true explicitly means a local development snapshot, not
a clean release from that commit. Public release Pages staging rejects true.
No automatic tag creation or release publication is implied by exporting
a catalog.

Image parts
------------

The catalog contains exactly three ordered parts. Each part records its
role, fixed same-origin relative path, byte offset, byte size and lowercase
SHA-256 digest:

* Bootloader: ``firmware/bootloader.bin`` at 0.
* Partition table: ``firmware/partition-table.bin`` at 32,768 (0x8000),
  exactly 3,072 bytes.
* Application: ``firmware/quadband_cores3.bin`` at 65,536 (0x10000),
  bounded by the existing 0x177000-byte application partition.

Validate all 4 KiB sector-rounded erase ranges as well as byte ranges.
The NVS and PHY region at 0x9000 through 0xffff is never a flash target.
Neither a padded merged binary nor a fourth part covering those gaps may
be substituted. No full-chip erase is allowed by the browser installer.
An incompatible existing partition layout requires an explicit separate
migration procedure, not an assertion that settings will be preserved.

Integrity and delivery
----------------------

The exporter validates generated flash arguments and build provenance
before copying the split images. It emits ``catalog.json``, the three
images, ``BUILD-INFO.json`` and ``SHA256SUMS.txt``. Checksums cover every
other emitted file exactly once. The browser verifies downloaded image
lengths and SHA-256 before writing, and verifies programmed bytes before
announcing success.

The generated website and matching payload are deployed together.
Image paths are fixed local relative names, not arbitrary URLs supplied
by release text. Publishing checks the expected release tag, source commit,
application version, clean-source status and all payload checksums.
File hashes provide integrity checks; authenticity still relies on the
trusted repository build and HTTPS hosting, not an independent firmware
signature.

Physical installation
----------------------

Users must explicitly confirm CoreS3 model, host output off and disconnected
Chameleon/host peripherals, select a port, and approve installation after
chip/security/capacity checks. A USB identifier or ESP32-S3 chip match is not
proof of the physical board model. Failed or interrupted installation must
not be presented as success or automatically restart the entire operation.
Browser and CI tests use mock serial devices and never flash hardware.
