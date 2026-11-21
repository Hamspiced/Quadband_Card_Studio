"""Merged Type2 behavior, reconciled with explicit guarded policy."""
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
import pytest
from quadband import bulk, cores3, formatting, hf, ndef, ntag, pcsc, type2
from quadband.common import CardError
from test_hf_families import NTAG, Classic4K
from test_hf import Card
from test_cores3 import TypedWire

RECORDS = [ndef.Record.uri('https://midwestgadgets.org')]
SAMPLES = {
    'Text': {'text': 'Hello — ünïcode'}, 'URL / URI': {'url': 'https://example.org/a?b=c'},
    'Custom URL / URI': {'url': 'myapp://open/item'}, 'Unit.Link': {'url': 'https://unit.link/x'},
    'Payment Link': {'url': 'https://pay.example/x'}, 'Social Networks': {'url': 'https://social.example/me'},
    'Video Link': {'url': 'https://video.example/v'}, 'File Link': {'url': 'https://example.org/f.pdf'},
    'Search': {'query': 'quadband card'}, 'Application': {'package': 'com.example.app'},
    'Mail': {'email': 'a@example.org', 'subject': 'Hi', 'body': 'Body'},
    'Contact': {'name': 'Ada Lovelace', 'phone': '+15551234567'}, 'Phone': {'phone': '+15551234567'},
    'Location': {'latitude': '42.3', 'longitude': '-83.0'}, 'SOS': {'name': 'Ada', 'phone': '555'},
    'Bluetooth': {'address': 'AA:BB:CC:DD:EE:FF', 'name': 'Speaker'},
    '3D Printing': {'material': 'PLA', 'brand': 'Acme'}, 'Wi-Fi': {'ssid': 'Net', 'password': 'password123'},
    'Data': {'mime': 'application/octet-stream', 'payload': 'DEADBEEF'}}


class Physical(pcsc.PCSC):
    """Exercise production PCSC identify, page guards and canonical APDUs without hardware."""
    def __init__(self, tag):
        super().__init__('simulated ACR122U')
        self.tag = tag
        self.connection = SimpleNamespace(getATR=lambda: tag.info.atr)
        self.apdus, self.reads, self.auths = [], [], []
        self.keys = {s: hf.FACTORY for s in range(40)}
        self.reject = None

    async def command(self, command, count=None):
        command = bytes(command)
        self.apdus.append(command)
        if command[:2] == b'\xff\xca':
            return self.tag.info.uid
        if command[:2] == b'\xff\0':
            return b'\xd5\x41\0' + self.tag.info.version
        page = command[3]
        if command[1] == 0xb0:
            self.reads.append(page)
            assert page + 4 <= self.tag.info.unit_count - 2
            return b''.join(self.tag.pages[p] for p in range(page, page+4))
        assert command[1] == 0xd6
        data = command[5:]
        self.tag.writes.append((page, data))
        if page == self.reject:
            raise CardError('Interrupted physical write')
        if self.tag.info.ntag:
            assert page == 3 or page in hf.eligible_units(self.tag.info.family)
            self.tag.pages[page] = bytes(a | b for a, b in zip(self.tag.pages[3], data)) if page == 3 else data
        else:
            assert page != 0
            self.tag.memory[page] = data
            if page % 4 == 3:
                self.keys[hf.sector_of(page)] = (data[:6], 0x60)
        return b''

    async def identify(self):
        if self.tag.info.classic:
            self.card = self.tag.info
            return self.card
        return await super().identify()

    async def authenticate(self, block, credential):
        from quadband.common import AuthenticationError
        self.auths.append((block, credential))
        if credential != self.keys[hf.sector_of(block)]:
            raise AuthenticationError('Wrong factory/canonical key')

    async def read_block(self, block, credential):
        await self.authenticate(block, credential)
        data = self.tag.memory[block]
        # Keys need not be readable after transition; verify access/GPB plus fresh auth.
        if block % 4 == 3 and credential != hf.FACTORY:
            data = bytes(6) + data[6:10] + bytes(6)
        return data

    async def reset_authentication(self):
        await asyncio.sleep(0)


async def reviewed(tag, records=RECORDS, overwrite=False, format=False):
    backend = Physical(tag)
    client = cores3.HFClient(backend)
    job = bulk.Job.freeze(tag.info.family, records, overwrite, format, bulk=False)
    review = await bulk.prepare(client, job)
    return await bulk.execute(client, review), backend


@pytest.mark.parametrize('family', hf.NTAG_LAYOUTS)
@pytest.mark.parametrize('kind', ndef.FIELDS)
async def test_all_merged_record_types_guarded_round_trip_or_capacity_reject(family, kind):
    record = ndef.build(kind, SAMPLES.get(kind, {}))
    assert ndef.decode(ndef.encode([record])) == [record]
    tag = NTAG(family)
    before = dict(tag.pages)
    if len(ndef.encode([record])) > type2.capacity(family):
        with pytest.raises(CardError, match='holds'):
            await reviewed(tag, [record], overwrite=True)
        assert not tag.writes
        return
    (snapshot, _), backend = await reviewed(tag, [record], overwrite=True)
    assert ntag.Type2NDEF(snapshot).records == [record]
    assert all(tag.pages[p] == before[p] for p in range(tag.info.unit_count)
               if p not in hf.eligible_units(family))
    assert all(p >= 4 and p < 4+tag.pages[3][2]*2 for p, _ in tag.writes)
    assert all(p+4 <= tag.info.unit_count-2 for p in backend.reads)


@pytest.mark.parametrize('family', hf.NTAG_LAYOUTS)
async def test_geometry_batched_read_count_and_no_fabricated_unknown_password_bytes(family):
    tag = NTAG(family)
    backend = Physical(tag)
    snapshot = await ntag.Client(backend).read()
    # ceil((real page count minus PWD/PACK) / 4), not a larger-family constant.
    assert len(backend.reads) == (tag.info.unit_count-2+3)//4
    assert set(snapshot.blocks) == set(range(tag.info.unit_count-2))
    assert b''.join(snapshot.blocks.values()) == b''.join(tag.pages[p] for p in range(tag.info.unit_count-2))


@pytest.mark.parametrize('family', hf.NTAG_LAYOUTS)
async def test_initialization_is_explicit_and_preserves_outside_cc(family):
    tag = NTAG(family)
    tag.pages[3] = bytes(4)
    for p in hf.eligible_units(family):
        tag.pages[p] = bytes(4)
    end = 4 + formatting.CC[family][2]*2
    for p in range(end, tag.info.unit_count-5):
        tag.pages[p] = b'KEEP'
    before = dict(tag.pages)
    with pytest.raises(CardError, match='explicit'):
        await reviewed(tag)
    assert not tag.writes
    outside_cc = end < tag.info.unit_count-5
    if outside_cc:
        with pytest.raises(CardError, match='already holds data'):
            await reviewed(tag, format=True)
        assert not tag.writes and tag.pages == before
    (snapshot, detail), _ = await reviewed(tag, overwrite=outside_cc, format=True)
    assert 'Formatted' in detail and ntag.Type2NDEF(snapshot).records == RECORDS
    assert tag.pages[3] == formatting.CC[family]
    assert all(tag.pages[p] == before[p] for p in range(tag.info.unit_count) if p < 3 or p >= end)
    format_cc = next(i for i, (p, _) in enumerate(tag.writes) if p == 3)
    assert tag.writes[format_cc-1] == (4, bytes.fromhex('0300fe00'))


async def test_merged_overwrite_asserts_encoded_tail_not_zero_message_page10():
    tag = NTAG('ntag215')
    tag.pages[3] = bytes(4)
    tag.pages[10] = bytes.fromhex('deadbeef')
    with pytest.raises(CardError, match='already holds data'):
        await reviewed(tag, format=True)
    assert not tag.writes
    (snapshot, _), _ = await reviewed(tag, overwrite=True, format=True)
    memory = ntag.Type2NDEF(snapshot).memory
    message = ndef.encode(RECORDS)
    assert memory[:len(message)+3] == bytes([3, len(message)])+message+b'\xfe'
    assert not any(memory[len(message)+3:])
    assert tag.pages[10] == memory[24:28]  # This page legitimately contains message tail.


@pytest.mark.parametrize('bad_cc', ['e1106d00', 'e1101200', '00100000', 'e1103e0f'])
async def test_nonzero_cc_is_never_upgraded_downgraded_or_repaired(bad_cc):
    tag = NTAG('ntag215')
    tag.pages[3] = bytes.fromhex(bad_cc)
    if bad_cc == 'e1101200':
        # Valid smaller advertised layouts are preserved, not upgraded.
        await reviewed(tag, overwrite=True, format=True)
        assert tag.pages[3] == bytes.fromhex(bad_cc) and all(p != 3 for p, _ in tag.writes)
    else:
        with pytest.raises(CardError):
            await reviewed(tag, overwrite=True, format=True)
        assert not tag.writes


@pytest.mark.parametrize('problem', ['static', 'dynamic', 'auth', 'incomplete', 'version'])
async def test_initialization_preflight_failure_is_zero_writes(problem):
    tag = NTAG('ntag215')
    tag.pages[3] = bytes(4)
    if problem == 'static':
        tag.pages[2] = b'\0\0\1\0'
    elif problem == 'dynamic':
        tag.pages[130] = b'\0\0\1\0'
    elif problem == 'auth':
        tag.pages[131] = b'\0\0\0\4'
    elif problem == 'version':
        tag.info = hf.CardInfo(tag.info.uid, family_hint='ntag215', version=b'\0'*8)
    backend = Physical(tag)
    client = cores3.HFClient(backend)
    if problem == 'incomplete':
        original = backend.read_pages
        async def short(page, count=1):
            return (await original(page, count))[:-1]
        backend.read_pages = short
    with pytest.raises(CardError):
        await bulk.prepare(client, bulk.Job.freeze('ntag215', RECORDS, True, True))
    assert not tag.writes


@pytest.mark.parametrize('family', hf.NTAG_LAYOUTS)
async def test_missing_exact_get_version_cannot_use_selected_dropdown_or_cc_identity(family):
    tag = NTAG(family)
    backend = Physical(tag)
    async def unknown():
        return None
    backend.get_version = unknown
    assert (await backend.identify()).family == 'unknown'
    with pytest.raises(CardError):
        await bulk.prepare(cores3.HFClient(backend), bulk.Job.freeze(family, RECORDS, True, True))
    assert not tag.writes


async def test_format_classic_factory_layout_preserves_manufacturer_and_publishes_directory_last():
    card = Card(False)
    card.memory[0] = bytes(range(16))
    backend = Physical(card)
    snapshot = await hf.ClassicClient(backend).read(hf.FACTORY, automatic=False)
    await backend.canonical_format(snapshot)
    assert card.memory[0] == bytes(range(16))
    assert card.memory[1]+card.memory[2] == formatting.directory()
    assert card.memory[3] == formatting.DIRECTORY_TRAILER
    assert all(card.memory[b] == formatting.DATA_TRAILER for b in range(7, 64, 4))
    assert [b for b, _ in card.writes][-3:] == [1, 2, 3]
    assert all((b, (formatting.DATA_TRAILER[:6], 0x60)) in backend.auths for b in range(7, 64, 4))
    area = hf.ClassicNDEF(await hf.ClassicClient(backend).read())
    assert area.records == [] and area.writable and area.capacity == 716


@pytest.mark.parametrize('problem', ['4k', 'keyb', 'access', 'data', 'missing', 'stale'])
async def test_classic_format_preflight_zero_mutations(problem):
    card = Classic4K() if problem == '4k' else Card(False)
    backend = Physical(card)
    if problem == 'keyb':
        card.memory[63] = card.memory[63][:10] + bytes(6)
    elif problem == 'access':
        card.memory[63] = bytes(16)
    elif problem == 'data':
        card.memory[62] = b'x'*16
    snapshot = await hf.ClassicClient(backend).read(hf.FACTORY, automatic=False)
    if problem == 'missing':
        snapshot.blocks.pop(62)
    elif problem == 'stale':
        card.memory[0] = b'x'*16
    with pytest.raises(CardError):
        await backend.canonical_format(snapshot)
    assert not card.writes


@pytest.mark.parametrize('family', ['classic1k', 'ntag213', 'ntag215', 'ntag216'])
async def test_shared_frozen_format_fixture_payload_capability_and_empty_response(family):
    fixture = json.loads((Path(__file__).resolve().parents[2]/'protocol/fixtures/ndef-bulk-v1.json').read_text())
    card = Card(False) if family == 'classic1k' else NTAG(family)
    if family == 'classic1k':
        card.info = hf.CardInfo(card.info.uid, atqa=b'\4\0', sak=8, family_hint='classic1k')
    if card.info.ntag:
        card.pages[3] = bytes(4)
        for p in hf.eligible_units(family):
            card.pages[p] = bytes(4)
    physical = Physical(card)
    snapshot = await (ntag.Client(physical).read() if card.info.ntag
                      else hf.ClassicClient(physical).read(hf.FACTORY, automatic=False))
    snapshot.card = card.info  # A bridge's proven identity includes ATQA/SAK, unlike a PCSC ATR.
    wire = TypedWire(card)
    session = cores3.Session(wire)
    spec = fixture['formatOperations'][family]
    try:
        await session.hello()
        async with session.lease(1) as endpoint:
            backend = cores3.HF(endpoint)
            with pytest.raises(CardError, match='advertise'):
                await backend.canonical_format(snapshot)
            assert not any(r.operation in (22, 23) for r in wire.sent)
        wire.caps['peripherals'][0]['capabilities'].append(spec['capability'])
        await wire.event()
        async with session.lease(1) as endpoint:
            backend = cores3.HF(endpoint)
            original = endpoint.request
            deadlines = []
            async def request(op, payload=b'', timeout=5):
                if op == spec['operation']:
                    deadlines.append(timeout)
                    expected = bytes([len(card.info.uid)]) + card.info.uid + b'\0' + formatting.snapshot_bytes(snapshot)
                    assert payload == expected and len(payload) == len(card.info.uid)+2+spec['snapshotBytes']
                    return b''
                return await original(op, payload, timeout)
            endpoint.request = request
            await backend.canonical_format(snapshot)
            assert deadlines == [120]
    finally:
        await session.close()


@pytest.mark.parametrize('value,is_removed', [(0x8010000c, True), (0x80100069, True),
                                            (0x80100066, False), (0x80100067, False), (0x80100016, False)])
def test_only_genuine_pcsc_no_card_or_removed_status_is_absence(value, is_removed):
    assert pcsc.removed(SimpleNamespace(hresult=value)) == is_removed
    assert pcsc.removed(SimpleNamespace(hresult=value-2**32)) == is_removed


@pytest.mark.parametrize('family', hf.NTAG_LAYOUTS)
async def test_single_target_keeps_more_than_three_records_and_extended_tlv(family):
    records = [ndef.Record.text(str(i)) for i in range(7)]
    (snapshot, _), _ = await reviewed(NTAG(family), records, overwrite=True)
    assert ntag.Type2NDEF(snapshot).records == records
    if family != 'ntag213':
        for length in (247, 248, 249, 300):
            records = [ndef.Record.text('x'*length)]
            (snapshot, _), _ = await reviewed(NTAG(family), records, overwrite=True)
            area = ntag.Type2NDEF(snapshot)
            size = len(ndef.encode(records))
            assert area.memory[1] == (size if size < 255 else 255)
            assert area.records == records


async def test_shorter_message_clears_old_bytes_and_identical_skips_writes():
    tag = NTAG('ntag215')
    await reviewed(tag, [ndef.Record.text('first '*30)], overwrite=True)
    tag.writes.clear()
    await reviewed(tag, overwrite=True)
    memory = ntag.Type2NDEF(await ntag.Client(tag).read()).memory
    assert not any(memory[len(ndef.encode(RECORDS))+3:])
    tag.writes.clear()
    (_, detail), _ = await reviewed(tag)
    assert 'Already correct' in detail and not tag.writes
