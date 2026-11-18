import asyncio
from contextlib import asynccontextmanager
import pytest
from quadband import hf, ndef, type2
from quadband.common import CardError
from quadband.service import StopFlag

ATR = bytes.fromhex('3B8F8001804F0CA0000003060300030000000068')
RECORDS = [ndef.Record.uri('https://midwestgadgets.org')]
SAMPLES = {
    'Text': {'text': 'Hello — ünïcode'}, 'URL / URI': {'url': 'https://example.org/a?b=c'},
    'Custom URL / URI': {'url': 'myapp://open/item'}, 'Unit.Link': {'url': 'https://unit.link/x'},
    'Payment Link': {'url': 'https://pay.example/x'}, 'Social Networks': {'url': 'https://social.example/me'},
    'Video Link': {'url': 'https://video.example/v'}, 'File Link': {'url': 'https://example.org/f.pdf'},
    'Search': {'query': 'quadband card'}, 'Application': {'package': 'com.example.app'},
    'Mail': {'email': 'a@example.org', 'subject': 'Hi', 'body': 'Body'}, 'Contact': {'name': 'Ada Lovelace', 'phone': '+15551234567'},
    'Phone': {'phone': '+15551234567'}, 'Location': {'latitude': '42.3', 'longitude': '-83.0'},
    'SOS': {'name': 'Ada', 'phone': '555'}, 'Bluetooth': {'address': 'AA:BB:CC:DD:EE:FF', 'name': 'Speaker'},
    '3D Printing': {'material': 'PLA', 'brand': 'Acme'}, 'Wi-Fi': {'ssid': 'Net', 'password': 'password123'},
    'Data': {'mime': 'application/octet-stream', 'payload': 'DEADBEEF'}}


class NTAG:
    """Simulated NTAG21x: OTP capability container, lock bytes, AUTH0, GET_VERSION and wrap-around-free reads."""
    def __init__(self, name='NTAG215', uid=bytes(range(1, 8)), formatted=True):
        self.spec = type2.TAGS[name]
        dyn = self.spec.dynamic_lock
        self.pages = [bytes(4) for _ in range(dyn+5)]
        self.pages[0], self.pages[1] = uid[:3]+bytes([0x88]), uid[3:7]
        if formatted:
            self.pages[3] = bytes([0xE1, 0x10, self.spec.cc_size, 0])
            if name == 'NTAG213':
                self.pages[4], self.pages[5] = bytes.fromhex('0103A00C'), bytes.fromhex('3403 00FE'.replace(' ', ''))
            else:
                self.pages[4] = bytes.fromhex('0300FE00')
        self.pages[dyn+1] = bytes.fromhex('000000FF')
        self.info = hf.CardInfo(uid, ATR)
        self.version = bytes([0, 4, 4, 2, 1, 0, self.spec.version, 3])
        self.writes, self.reads, self.reject = [], [], None

    async def identify(self):
        return self.info

    async def present(self):
        return True

    async def get_version(self):
        return self.version

    async def read_pages(self, page):
        self.reads.append(page)
        if page+4 > len(self.pages):
            raise CardError('Card access failed (6300); check key, permissions and placement.')
        return b''.join(self.pages[page:page+4])

    async def write_page(self, page, data):
        assert len(data) == 4 and 3 <= page < len(self.pages)
        self.writes.append(page)
        if page == self.reject:
            raise CardError('Simulated write failure')
        self.pages[page] = bytes(a | b for a, b in zip(self.pages[3], data)) if page == 3 else bytes(data)


def client(tag):
    return type2.Type2Client(tag)


def test_specs_follow_nxp_memory_maps():
    assert [(s.name, s.user, s.area, s.end, s.dynamic_lock) for s in type2.TAGS.values()] == [
        ('NTAG213', 144, 144, 40, 40), ('NTAG215', 504, 496, 128, 130), ('NTAG216', 888, 872, 222, 226)]
    assert [type2.capacity(t) for t in type2.TAGS] == [137, 492, 868]
    assert list(type2.tag_types().values()) == ['MIFARE Classic 1K', 'NTAG213', 'NTAG215', 'NTAG216']


@pytest.mark.parametrize('name', list(type2.TAGS))
async def test_factory_tag_is_encoded_without_touching_uid_locks_or_config(name):
    tag = NTAG(name)
    before = list(tag.pages)
    snapshot, formatted, written = await client(tag).encode_tag(name, RECORDS)
    assert not formatted and written
    assert type2.Type2NDEF(snapshot).records == RECORDS
    config = range(tag.spec.dynamic_lock, len(tag.pages))
    assert all(tag.pages[p] == before[p] for p in [0, 1, 2, 3, *config])
    assert all(p >= 4 and p < tag.spec.end for p in tag.writes)
    message = ndef.encode(RECORDS)
    start = 5 if name == 'NTAG213' else 0  # NTAG213 ships with a Lock Control TLV that must be preserved
    memory = b''.join(tag.pages[4:tag.spec.end])
    assert memory[:start] == b'\1\3\xa0\x0c\x34'[:start]
    assert memory[start:start+2+len(message)] == bytes([3, len(message)])+message and memory[start+2+len(message)] == 0xFE


@pytest.mark.parametrize('name', list(type2.TAGS))
async def test_unformatted_tag_gets_capability_container_and_empty_ndef(name):
    tag = NTAG(name, formatted=False)
    snapshot, formatted, written = await client(tag).encode_tag(name, RECORDS)
    assert formatted and written
    assert tag.pages[3] == bytes([0xE1, 0x10, type2.TAGS[name].cc_size, 0])
    assert type2.Type2NDEF(snapshot).records == RECORDS
    assert tag.writes[0] == 3 and 0 not in tag.writes and 2 not in tag.writes


async def test_format_leaves_empty_tag_before_records_are_written():
    tag = NTAG('NTAG216', formatted=False)
    snapshot = await client(tag).read('NTAG216')
    formatted = await client(tag).format_ndef(snapshot)
    assert type2.Type2NDEF(formatted).records == [] and b''.join(tag.pages[4:6]) == bytes.fromhex('0300FE0000000000')


async def test_existing_data_needs_explicit_overwrite():
    tag = NTAG('NTAG215', formatted=False)
    tag.pages[10] = b'\xde\xad\xbe\xef'
    with pytest.raises(CardError, match='already holds data'):
        await client(tag).encode_tag('NTAG215', RECORDS)
    assert not tag.writes
    _, formatted, _ = await client(tag).encode_tag('NTAG215', RECORDS, erase_data=True)
    assert formatted and tag.pages[10] == bytes(4)


async def test_foreign_ndef_message_is_replaced_and_identical_is_skipped():
    tag = NTAG('NTAG215')
    await client(tag).encode_tag('NTAG215', [ndef.Record.text('first '*30)])
    tag.writes.clear()
    snapshot, _, written = await client(tag).encode_tag('NTAG215', RECORDS)
    assert written and type2.Type2NDEF(snapshot).records == RECORDS
    # Old message bytes beyond the terminator are cleared.
    assert not any(b''.join(tag.pages[4:tag.spec.end])[len(ndef.encode(RECORDS))+3:])
    tag.writes.clear()
    _, _, written = await client(tag).encode_tag('NTAG215', RECORDS)
    assert not written and not tag.writes


async def test_selected_type_must_match_the_tag():
    tag = NTAG('NTAG215')
    with pytest.raises(CardError, match='This is an NTAG215, but NTAG213 is selected'):
        await client(tag).encode_tag('NTAG213', RECORDS)
    assert not tag.writes
    tag.version = bytes([0, 4, 4, 2, 1, 0, 0x0B, 3])
    with pytest.raises(CardError, match='not an NTAG213, NTAG215 or NTAG216'):
        await client(tag).read()
    classic = NTAG()
    classic.info = hf.CardInfo(b'\1\2\3\4', ATR)
    with pytest.raises(CardError, match='not an NTAG'):
        await client(classic).read('NTAG215')


async def test_size_is_checked_for_every_type():
    assert type2.check_fit('NTAG216', [ndef.Record.text('x'*800)]) > 800
    with pytest.raises(CardError, match='NTAG213 holds 137'):
        type2.check_fit('NTAG213', [ndef.Record.text('x'*200)])
    tag = NTAG('NTAG213')
    with pytest.raises(CardError, match='holds'):
        await client(tag).encode_tag('NTAG213', [ndef.Record.text('x'*200)])
    assert not tag.writes
    big = [ndef.Record.text('long '*160)]   # needs the three-byte TLV length
    snapshot, _, _ = await client(NTAG('NTAG216')).encode_tag('NTAG216', big)
    assert type2.Type2NDEF(snapshot).records == big


@pytest.mark.parametrize('name', list(type2.TAGS))
async def test_short_message_does_not_read_the_whole_memory(name):
    tag = NTAG(name)
    await client(tag).encode_tag(name, RECORDS)
    # head, first data chunk, config, then only the written pages again to verify.
    assert len(tag.reads) <= 7 and len(tag.writes) <= 8
    tag.reads.clear()
    snapshot = await client(tag).read(name)
    assert not snapshot.complete and len(tag.reads) <= 4
    assert type2.Type2NDEF(snapshot).records == RECORDS
    full = await client(tag).read(name, full=True)
    assert full.complete and len(tag.reads) > 20


@pytest.mark.parametrize('scenario', ['static', 'dynamic', 'password', 'cc-read-only'])
async def test_locked_or_protected_tags_are_refused_without_writing(scenario):
    tag = NTAG('NTAG215')
    dyn = tag.spec.dynamic_lock
    if scenario == 'static':
        tag.pages[2] = bytes([0, 0, 0x10, 0])
    elif scenario == 'dynamic':
        tag.pages[dyn] = bytes([1, 0, 0, 0])
    elif scenario == 'password':
        tag.pages[dyn+1] = bytes.fromhex('00000004')
    else:
        tag.pages[3] = bytes([0xE1, 0x10, tag.spec.cc_size, 0x0F])
    with pytest.raises(CardError, match='locked|password|read-only'):
        await client(tag).encode_tag('NTAG215', RECORDS)
    assert not tag.writes


async def test_one_time_programmable_capability_container_cannot_be_downgraded():
    tag = NTAG('NTAG215', formatted=False)
    tag.pages[3] = bytes([0xE1, 0x10, 0x6D, 0])  # already larger than a 215 may declare
    with pytest.raises(CardError, match='permanently set'):
        await client(tag).encode_tag('NTAG215', RECORDS)
    assert not tag.writes
    tag.pages[3] = bytes([0xE1, 0x10, 0x12, 0])  # a 213-sized CC can only gain bits: upgrade is possible
    await client(tag).encode_tag('NTAG215', RECORDS)
    assert tag.pages[3] == bytes([0xE1, 0x10, 0x3E, 0])


async def test_interrupted_write_leaves_a_valid_empty_message_and_is_not_retried():
    tag = NTAG('NTAG215')
    tag.reject = 7
    with pytest.raises(CardError, match='not verified'):
        await client(tag).encode_tag('NTAG215', [ndef.Record.text('changed '*30)])
    assert tag.pages[4][:2] == b'\3\0'
    assert tag.writes.count(7) == 1


async def test_tag_swapped_mid_write_is_detected():
    tag = NTAG('NTAG215')
    original = tag.write_page
    async def swap(page, data):
        await original(page, data)
        tag.info = hf.CardInfo(bytes(range(9, 16)), ATR)
    tag.write_page = swap
    with pytest.raises(CardError, match='changed'):
        await client(tag).encode_tag('NTAG215', [ndef.Record.text('x'*100)])
    assert len(tag.writes) == 1


@pytest.mark.parametrize('kind', list(ndef.FIELDS))
@pytest.mark.parametrize('name', ['NTAG213', 'NTAG215', 'NTAG216'])
async def test_every_record_type_round_trips_or_is_rejected_for_size(kind, name):
    record = ndef.build(kind, SAMPLES.get(kind, {}))
    assert ndef.decode(ndef.encode([record])) == [record]
    tag = NTAG(name)
    if len(ndef.encode([record])) > type2.capacity(name):
        with pytest.raises(CardError, match='holds'):
            await client(tag).encode_tag(name, [record])
        assert not tag.writes
        return
    snapshot, _, _ = await client(tag).encode_tag(name, [record])
    assert type2.Type2NDEF(snapshot).records == [record]
    assert [r.display() for r in type2.Type2NDEF(snapshot).records] == [record.display()]


async def test_every_record_type_fits_on_ntag216_together():
    records = [ndef.build(k, SAMPLES.get(k, {})) for k in ('Text', 'URL / URI', 'Phone', 'Location', 'Bluetooth', 'Wi-Fi', 'Data')]
    snapshot, _, _ = await client(NTAG('NTAG216')).encode_tag('NTAG216', records)
    assert type2.Type2NDEF(snapshot).records == records


class Station:
    """Scripted PC/SC reader: each step is a tag, None (absent) or an exception."""
    def __init__(self, steps, stop):
        self.steps, self.stop, self.current, self.backend = list(steps), stop, None, self

    async def present(self):
        if self.steps:
            self.current = self.steps.pop(0)
        else:
            self.stop.set()
        if isinstance(self.current, Exception):
            raise self.current
        return self.current is not None

    async def identify(self):
        return await self.current.identify()

    async def get_version(self):
        return await self.current.get_version()

    async def read_pages(self, page):
        return await self.current.read_pages(page)

    async def write_page(self, page, data):
        return await self.current.write_page(page, data)


async def drive(steps, tag, records=RECORDS, **options):
    stop, results, phases = StopFlag(), [], []
    reader = Station(steps, stop)
    @asynccontextmanager
    async def factory():
        yield reader
    await asyncio.wait_for(hf.encode_forever(factory, records, hf.FACTORY, stop, lambda *a: phases.append(a), lambda *a: results.append(a),
                                             .001, settle=0, tag=tag, **options), 5)
    return results, phases


async def test_bulk_encodes_ntags_once_each_and_requires_removal():
    a, b = NTAG('NTAG215', bytes(range(1, 8)), formatted=False), NTAG('NTAG215', bytes(range(21, 28)))
    results, phases = await drive([None, a, a, None, b, b, None, a], 'NTAG215')
    assert [r[0] for r in results] == [True, True] and 'formatted' in results[0][1]
    assert type2.Type2NDEF(await client(a).read()).records == RECORDS
    assert type2.Type2NDEF(await client(b).read()).records == RECORDS
    assert [p[0] for p in phases][:3] == ['ready', 'writing', 'done'] and 'warn' in [p[0] for p in phases]


async def test_bulk_reports_wrong_type_and_continues():
    wrong, good = NTAG('NTAG216', bytes(range(1, 8))), NTAG('NTAG215', bytes(range(21, 28)))
    results, phases = await drive([wrong, None, good], 'NTAG215')
    assert [r[0] for r in results] == [False, True] and 'NTAG216' in results[0][1]
    assert not wrong.writes and any(p[0] == 'error' for p in phases)


async def test_bulk_rejects_oversized_records_before_touching_tags():
    tag = NTAG('NTAG213')
    with pytest.raises(CardError, match='holds 137'):
        await drive([tag], 'NTAG213', [ndef.Record.text('x'*300)])
    assert not tag.writes
