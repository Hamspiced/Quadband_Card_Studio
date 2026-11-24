CoreS3 bridge protocol, version 1
=================================

This contract is shared by firmware, native macOS and portable clients.
``cores3-bridge-v1.json`` is its machine-readable manifest.
``fixtures/cores3-v1.json`` contains golden wire, geometry and profile examples.
The fixture generator is not a desktop hardware dependency.

Transport and framing
---------------------

Use the dedicated service/characteristic UUIDs in the manifest, not the X6 or
Chameleon Nordic UART service. RX is an encrypted, authenticated GATT
write-with-response characteristic. TX is an encrypted, authenticated
indication characteristic; its CCCD subscription is required before HELLO.
Advertise the service and the local name ``Quadband CoreS3``. Permit one
authenticated bonded client. The screen must authorize pairing and display
the passkey; a local action opens a bounded pairing window or clears bonds.
Do not store card contents, user keys or passwords in NVS or SD.

One frame is the following 26-byte header, its payload and a four-byte CRC.
All multibyte header integers are little-endian:

* 0: magic, two bytes ``51 42`` (ASCII QB).
* 2: protocol version, one byte, 1.
* 3: kind, one byte: request 0, response 1, event 2.
* 4: session nonce, unsigned 64-bit.
* 12: request ID, unsigned 32-bit.
* 16: channel, unsigned 8-bit.
* 17: operation, unsigned 8-bit.
* 18: status, unsigned 16-bit (zero for requests).
* 20: peripheral generation, unsigned 32-bit (zero for control).
* 24: payload length, unsigned 16-bit, at most 2048.
* 26: payload.
* After the payload: unsigned 32-bit little-endian CRC-32/ISO-HDLC of the
  complete header plus payload; equivalent to Python ``zlib.crc32``.

Maximum frame size is 2078 bytes. A BLE attribute value may contain a
fragment, a full frame or several frames. Reassemble by length, never by
delimiter bytes appearing in the payload. Bound a receive buffer to 8312
bytes; reject invalid magic/version/kind/length/CRC and terminate the session
on framing loss rather than scanning through arbitrary bytes to a new magic.
Start at 20 bytes per BLE write; respect a smaller platform limit and use a
larger negotiated payload only when both ends support it. Never execute a
request until it is completely received and validated.

Responses echo request ID, channel and operation. Error payloads are optional
UTF-8 diagnostic text, never an alternate success layout. Events use request
ID zero. Non-HELLO requests need the current nonzero session nonce and a
nonzero request ID. Request IDs are monotonically increasing per session;
duplicates must not execute again. Refuse wraparound; establish a new session.
Firmware must reserve an accepted request ID before queuing the operation.

Only one physical operation may be pending in a desktop bridge session;
firmware must bound its worker queue and reject additional work with BUSY.
This still permits unsolicited peripheral-change events and additional
inventory reports. In-flight writes are never replayed after a timeout,
disconnect or cancellation. A client must expose an unverified outcome if
the link fails after transmission. Clear reassembly, transient credentials,
pending callbacks and queued work on disconnect.

Channels, capabilities and sessions
-----------------------------------

Channels are control 0, Grove HF 1, Grove UHF 2 and Chameleon 3. Channel 3
uses one locally selected USB or Bluetooth transport; it is not a separate
reader or ownership lease for each transport.

Control operations:

* HELLO 1: request has session/generation zero and exactly eight opaque
  client-nonce bytes. Response header carries a newly generated nonzero
  64-bit session nonce. Response payload is the capabilities JSON below,
  including ``clientNonce`` as the lowercase hex of those eight bytes.
  Only accept HELLO on an authenticated link and do not replace a busy
  session. A new BLE link requires a new HELLO.
* CAPABILITIES 2: empty request, same JSON response under the active session.
* PING 3: echo a payload of at most 64 bytes.
* CANCEL 4: payload is a little-endian uint32 target request ID. The control
  response acknowledges cancellation acceptance, not undoing a write.
  Firmware must check the cancel flag between physical commands and not
  abandon a physical transaction in a way that lets its late reply satisfy
  the next request. Disconnect is the fail-closed client fallback.

All peripheral requests must carry that channel's current nonzero generation.
Increment generation on attach, detach, driver fault or replacement. Reject
stale generations before any physical I/O. When a raw-device transaction
times out, drain/finish it and establish an identification barrier before
advertising the recovered channel with a new generation. Do not resume
queued operations or accept late results into a new session/generation.

Capabilities use UTF-8 JSON with this exact shape (additional fields may
be ignored; all shown fields are required)::

   {
     "protocol": 1,
     "deviceId": "stable non-secret board identifier",
     "name": "Quadband CoreS3",
     "firmware": "0.1.0",
     "clientNonce": "1122334455667788",
     "peripherals": [
       {"channel": 1, "generation": 1, "present": true,
        "kind": "rfid2", "capabilities": ["identify", "classic1k", "classic4k", "ntag21x"]},
       {"channel": 2, "generation": 1, "present": true,
        "kind": "jrd4035", "capabilities": ["m100"]},
       {"channel": 3, "generation": 1, "present": false,
        "kind": "chameleon", "capabilities": []}
     ]
   }

Always include all three peripheral slots. ``kind`` for HF is ``rfid``,
``rfid2`` or ``unknown``. Only advertise implemented and detected
capabilities. ``ntag_auth`` is an additional capability when supplied-password
authentication is implemented. Unknown HF silicon is not write-capable.
The present UHF slot advertises ``m100``; the present Ultra slot
advertises ``chameleon`` only after its actual model/command list is checked.
Additional Chameleon capability strings such as ``classic4k`` and
``t5577_password_free`` must also reflect real command support.

Firmware 0.2.0 adds optional mode metadata without changing version-1 framing:
``mode`` is ``home``, ``connected`` or ``standalone``; ``owner`` is respectively
``none``, ``desktop`` or ``local``; ``modePending`` is a boolean. The new
firmware always sends all three. Clients validate known values and reject
contradictory ownership. When these fields are entirely absent, legacy
firmware retains its previous Connected semantics.

Only Connected with ``modePending=false`` permits peripheral requests.
Home, Standalone and pending transitions still permit authenticated control
operations, but reject channels 1--3 with BUSY before physical execution.
Mode selection is local, not a remote takeover operation.

On a confirmed transition, block new physical admission, cancel queued work
and check stop between physical commands. Finish the current transaction
without replay, release RF and verify original Chameleon mode restoration,
then commit the new owner and advance every peripheral generation. Failed
release falls back to Home with USB quarantined and a visible restart warning.
Clients retain response correlation for an already-transmitted command during
the pending-mode event; they block subsequent work and invalidate idle leases.
They invalidate selections/previews when the completed transition advances
generations. A mode change does not itself imply BLE disconnection.

An event on control channel 0, operation 128, generation zero carries a
complete capabilities JSON after peripheral state changes. A client invalidates
reader selections, active leases and write previews affected by a generation
change. It does not silently select a replacement reader.

HF operations
-------------

Operations 16 through 21 apply only to channel 1. All integer fields in HF
payloads are little-endian. A UID prefix is ``uidLength:u8`` then that many
UID bytes; supported lengths are 4, 7 or 10. Every read/write request includes
the expected UID and must identify/select that same physical card first.
Card changes and stale bytes are explicit errors, never implicit retargeting.

IDENTIFY 16 has an empty request and UTF-8 JSON response::

   {"uid":"01020304","atqa":"0400","sak":8,"family":"classic1k",
    "blockCount":64,"pageCount":0,"version":""}

Hex strings are lowercase or uppercase on input and normalized on output.
Families are ``classic1k``, ``classic4k``, ``ntag213``, ``ntag215``,
``ntag216`` and ``unknown``. SAK is an integer, ATQA is two bytes in wire
order. NTAG version is the eight-byte GET_VERSION result in hex. Unknown
families have zero block/page counts and cannot be written. Classic 4K has
256 blocks. NTAG total page counts are 45, 135 and 231 respectively.

CLASSIC_READ 17 request: UID prefix, ``block:u16``, ``keyType:u8`` (0x60 or
0x61), ``key[6]``. Response is exactly 16 bytes.

CLASSIC_WRITE 18 request: CLASSIC_READ payload followed by
``expectedPrevious[16]`` and ``desired[16]``. Firmware checks identity,
geometry, key and access bits, compares previous bytes, sends one physical
write and reads it back. Response is exactly the 16 verified bytes.
Manufacturer block 0 and every sector trailer are forbidden.

NTAG_READ 19 request: UID prefix, ``page:u16``, ``count:u8`` (1–16 pages),
then NTAG authentication. Response contains exactly ``count * 4`` bytes.
Reads may include readable metadata needed for permission validation, but
must stay within the identified part's real page count and must not turn
returned password masking into claimed known credentials.

NTAG_WRITE 20 request: UID prefix, ``page:u16``, NTAG authentication,
``expectedPrevious[4]``, ``desired[4]``. Firmware checks exact family,
identity, CC access, static/dynamic locks and authentication boundaries;
only known writable user pages are eligible. Compare previous bytes, write
once and read back; return exactly four verified bytes. Never write UID,
manufacturer, CC, static/dynamic locks, password/PACK or configuration pages.
Unknown layouts and unverified permission data are read-only.

NTAG authentication is ``mode:u8``: zero has no following bytes, one has a
four-byte supplied password, two has password[4] plus expected PACK[2].
Authenticate only with an explicit supplied credential. When mode two is
used, validate PACK. Do not guess passwords, change protection, or persist
the credential. The desktop disables this path unless ``ntag_auth`` is
advertised.

RESET_AUTH 21 has an empty request and response; reset local card
authentication/RF state without changing card contents.

Canonical formatting extension
------------------------------

Optional operations CLASSIC_FORMAT_1K 22 and NTAG_INITIALIZE 23 apply only
to channel 1. Advertise respectively ``classic1k_format`` and
``ntag_initialize`` only when the complete guarded implementation exists.
Older firmware without the capability must not receive these operations.
Generic CLASSIC_WRITE, NTAG_WRITE and raw Chameleon transactions retain
their protected-address prohibitions.

Both requests contain the expected UID prefix, ``flags:u8`` and a complete
expected readable snapshot in ascending unit order. Flags may be zero or
one: bit zero explicitly authorizes erasing existing different user data;
all other bits are rejected. This flag does not bypass geometry, credential,
lock, permission, stale-data or formatting checks. Expected bytes bind a
previously displayed per-target review, not a desired arbitrary memory image.
These operations have empty success responses and a bounded client deadline
of 120 seconds; cancellation remains cooperative between physical commands.
Never automatically retry a timeout or lost response.

For CLASSIC_FORMAT_1K the snapshot is exactly 64 blocks of 16 bytes
(1024 bytes). Identify actual Classic1K geometry, compare manufacturer/data
bytes, authenticate every sector with factory key A FFFFFFFFFFFF, and
validate factory access bytes FF0780 plus factory key B before the first
mutation. Trailer key A is masked in read-back; compare trailer bytes 6--15,
not unreadable key A bytes. Require every data block to be zero unless
overwrite is authorized. Block 0 is compared but never written.
Recheck identity and expected previous bytes before each mutation.

The only permitted result is the fixed NFC Forum MAD1 layout: sectors 1--15
are assigned AID 03E1, MAD CRC starts at C7 using polynomial 1D, and the data
area starts with 0300FE followed by zero padding. MAD information byte 1
is 01; calculate its CRC over bytes 1--31. Data-sector trailers are
D3F7D3F7D3F77F078840FFFFFFFFFFFF; the sector-zero trailer is
A0A1A2A3A4A5787788C1FFFFFFFFFFFF. No caller-specified key, access bytes,
trailer or allocation is accepted. Preflight all sectors, stage an empty
NDEF message, transition/verify data sectors, and publish the MAD directory
and sector-zero trailer last. Verify transitioned sectors by fresh
authentication with the canonical key and access/GPB read-back.
Classic4K, partially formatted and non-factory cards are rejected.
In particular, reject sector-zero GPB C1 or C2 even with overwrite enabled;
the formatter does not repair previously published MAD layouts.

For NTAG_INITIALIZE the snapshot contains pages zero through the second
configuration page inclusive: exactly ``(pageCount - 2) * 4`` bytes for the
identified part (172/532/916 bytes). Password and PACK pages are excluded.
Require the exact eight-byte GET_VERSION identity for NTAG213/215/216,
an all-zero CC, no static/dynamic locks, AUTH0 FF and no supplied-password
mode. Compare all expected readable bytes before mutation.
The canonical CC is E1101200, E1103E00 or E1106D00 respectively.
Only CC page 3 and user pages within that CC's declared area may change;
initialize that area to 0300FE followed by zero padding. Preserve physical
user pages outside the declared area and all UID/lock/config/password/PACK
pages. Nonzero existing user data requires overwrite authorization.
Existing nonzero CC values, including incompatible OTP layouts, are never
upgraded, downgraded or repaired by this operation.

After either operation, freshly read the target before the separately
guarded length-last record write. A bulk job must retain the reviewed
identity and frozen records across these steps. A failure after any attempted
mutation returns WRITE_UNVERIFIED, including failed verification, cancellation
and identity/generation loss. It never reports an unchanged card or resumes
a partial format automatically.

Raw device transaction
----------------------

TRANSACT 32 applies to UHF channel 2 or Chameleon channel 3. The request
payload is exactly one complete existing device-protocol command frame.
The response payload is one complete matching device frame, including its
own checksum/status. Device error replies stay inside a successful bridge
transaction; bridge transport errors use bridge status instead.

Additional UHF inventory notifications may use channel 2 operation 32
events with the current generation and a complete M100 notification frame.
Do not emit the same physical frame as both the response and an event.
Multiple tag notifications and no-tag replies are valid. The desktop
adapters preserve existing M100 and Chameleon parser semantics.

UHF command allowlist: 0x03 identify, 0x08 get region, 0x0B/0x0C selection,
0x12 select mode, 0x22 single inventory, 0x28 stop, 0x39 read, 0x49 write,
0xB6/0xB7 power. Check payload shape, address/count bounds and bank restrictions
before sending. Power is 1800–2600 hundredths dBm. Writes are at most 32
words, User or EPC after its CRC word; region/lock/kill/Reserved/TID writes
are never forwarded.

Chameleon allowlist for either transport in the existing application: 1000, 1001, 1002,
1017, 1033, 1035, 2000, 2008, 2009, 3000, 3002, 3004, 3010, 3014,
3016, 3019 and 3021. Validate the existing payload layouts; reject malformed
requests, emulation/flash commands, all legacy password-setting LF writers,
and unimplemented raw ISO14443 command forwarding. HF writes use known
Classic geometry; bind them to a preceding identified card/read and recheck
identity before mutation. Never permit block 0 or trailers. Command 3016
must satisfy the existing password-free T5577 policy: exactly 11 bytes,
block 0–6, final six bytes zero, and PWD bit clear in config writes.

The selected Chameleon continues to advertise its real command capabilities
through command 1035; neither firmware nor desktop invents missing command
support. NTAG support in version 1 is supplied through the Grove HF module;
do not claim Chameleon NTAG writes until an explicitly guarded adapter
exists. Preserve the Chameleon's original device mode on normal release.
HF/LF operations use one exclusive lease; never interleave them.

A local change of Chameleon peer or USB/Bluetooth transport requires idle,
verified cleanup and a new peripheral generation even when model and
capabilities are identical. A Bluetooth reader link is distinct from the
authenticated desktop bridge link. Losing either link must not misroute
responses or reuse the other link's authorization. No automatic transport
fallback, reconnection of physical operations or replay is permitted.
Selecting Bluetooth does not authorize USB VBUS or change the command
allowlist, framing, channel number or desktop ownership policy.

Status values
-------------

0 OK; 1 BAD_FRAME; 2 BAD_VERSION; 3 NOT_AUTHORIZED; 4 BAD_SESSION;
5 UNAVAILABLE; 6 STALE_GENERATION; 7 BUSY; 8 TIMEOUT; 9 CANCELLED;
10 NO_CARD; 11 AUTH_FAILED; 12 UNSUPPORTED; 13 INVALID_ARGUMENT;
14 IO_ERROR; 15 WRITE_UNVERIFIED; 16 PERMISSION_DENIED; 17 STALE_DATA.

NO_CARD maps to presence false, AUTH_FAILED to authentication failure.
Do not disguise an RF/transport failure as card removal. Errors after a
sent write must never be displayed as an unchanged tag or verified success.

Geometry and saved profiles
---------------------------

Classic 1K: 16 four-block sectors, 64 blocks, 47 data blocks.
Classic 4K: sectors 0–31 have four blocks; sectors 32–39 have sixteen.
Sector 32 starts at block 128 and sector 39 ends with trailer 255.
Large-sector data access groups are offsets 0–4, 5–9 and 10–14.
Classic 4K has 215 ordinary data blocks after excluding block 0 and trailers.
MAD/MAD2 are not part of NDEF payload allocation; validate the directory,
its CRC/version and the corresponding sector keys/access bits.

NTAG213: 45 total pages; user pages 4–39; dynamic lock page 40.
NTAG215: 135 total pages; user pages 4–129; dynamic lock page 130.
NTAG216: 231 total pages; user pages 4–225; dynamic lock page 226.
Configuration starts at the page following dynamic locks. User area size
and NDEF CC-advertised size are not interchangeable: derive usable NDEF
capacity from validated CC/TLVs within real physical user bounds.

Legacy ``quadband.classic1k.data`` version 1 profiles remain byte-compatible.
Do not silently reinterpret them for a different family. New profiles use::

   {"format":"quadband.card.data","version":2,"family":"classic4k",
    "geometry":{"unitSize":16,"unitCount":256},
    "name":"Classic 4K data","createdAt":"2026-11-02T16:00:00Z",
    "blocks":[{"number":1,"hex":"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"}]}

The example abbreviates ``blocks``: real files must contain every eligible
unit exactly once (215 for Classic 4K; 36/126/222 user pages for NTAG).
NTAG families use ``unitSize`` 4 and their total page counts above.
Profiles contain data only: no source UID, key, password, PACK, manufacturer
block, trailer, lock, CC or configuration pages. Require matching family
and geometry on import and preflight every target unit before the first
write. Preserve unknown formats by rejecting them, not guessing geometry.

Implementation and validation
-----------------------------

Use one shared desktop session with endpoint leases, not separate BLE
connections per workspace. Freeze this contract before parallel work and
coordinate any necessary change through the lead. Test the golden frame,
geometry and profile fixtures without hardware in all three implementations.
Never flash a device or perform a physical card write as an automated test.

USB host VBUS output starts disabled. A local per-boot action authorizes
host power only after the computer programming cable is disconnected and
the board has suitable independent power. Use the board power API's
protection; never persist an unconditional USB-output-on boot preference.
