CoreS3 browser installer
========================

Branded Web Serial installer for the M5Stack CoreS3 ESP32-S3 / 16 MB target.
This is not a general-purpose flasher for Core2, Basic/Fire or arbitrary
ESP32-S3 boards. Browser tests use mocks and never access physical ports.

Build and test
---------------

Use Node.js 22 or later from this directory::

    npm ci
    npm test
    npm run build
    npx playwright install chromium
    npm run test:browser

The browser tests serve the built site under the repository URL prefix,
check dark/light/mobile layouts and inject mock serial access. On Linux CI,
``npx playwright install --with-deps chromium`` supplies the required browser
libraries. A local installed Chrome can instead be selected through
``PW_CHROME_PATH``; do not use a personal browser profile for tests.

Generate real firmware payloads
-------------------------------

Activate the matching ESP-IDF SDK and build firmware with
``firmware/cores3/build.py`` from the repository root. Then export from
that root with the actual release tag, application version and source SHA::

    python firmware/cores3/tools/export_web.py --output .build/web-payload \
      --release-tag v1.0.2 --application-version 1.0.2 --commit SOURCE_COMMIT

The tag/application version must agree, and SOURCE_COMMIT must be the full
checked-out commit. The command refuses stale source/artifact hashes, dirty
release sources or SDK provenance, malformed images and unsupported layouts.
It never builds, opens serial devices or flashes anything.

For explicitly local uncommitted work, ``--allow-dirty`` marks the catalog as
a development snapshot. Such payloads are not public releases and are
rejected by release Pages staging. ``QB_REAL_PAYLOAD`` can point tests at
that exported directory for real-image verification and a labelled preview.

A release deployment builds ``dist/`` first, then validates and stages the
six exported payload files with ``scripts/prepare_pages.py``. Do not replace
that validation by blindly copying arbitrary download files. The website
and matching catalog/binaries must be deployed together.

Flash safety
-------------

Users confirm physical board and safe USB power, choose a port, inspect it,
and separately approve installation. Chip, capacity and security checks
precede writes. Exactly three sector-checked split images are permitted;
NVS / PHY at 0x9000–0xffff and full-chip erase are excluded. Supported
partitions must match exactly, or the whole flash must be genuinely blank.
Hashes are checked before writing and programmed bytes are verified before
success/reset. A failed serial write releases its stream lock; cancellation
cannot start a second writer or be overwritten by a success message.

Unknown/partially written partition layouts require the release bundle's
manual split-image recovery instructions. Do not erase saved data merely
to bypass the browser's refusal.

Hosting and dependencies
------------------------

See ``../docs/RELEASING.txt`` for the one-time GitHub Pages setup and
release-triggered pipeline. No live website or release is created by these
local build commands. Runtime libraries are pinned and bundled locally;
``tools/notices.mjs`` includes their installed notices in the built site.
Source code and dependency locks are included in public source archives.
