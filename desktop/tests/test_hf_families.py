import copy
import json
import pytest
from quadband import hf, ntag, ndef
from quadband.common import CardError
from test_hf import Card
from test_cores3 import GOLDEN


@pytest.mark.parametrize('family', ['classic1k', 'classic4k'])
def test_geometry_matches_frozen_contract(family):
    geometry = GOLDEN['geometries'][family]
    for sector in geometry:
        blocks = hf.sector_blocks(sector['sector'])
        assert blocks[0] == sector['firstBlock'] and len(blocks) == sector['blockCount']
        assert blocks[-1] == sector['trailer']
        for index, group in enumerate(sector['accessGroups']):
            assert all(hf.access_group(b) == index and hf.sector_of(b) == sector['sector'] for b in group)
    assert len(hf.eligible_units(family)) == (47 if family == 'classic1k' else 215)
    assert not any(hf.data_block(p['trailer'], 64 if family == 'classic1k' else 256) for p in geometry)


@pytest.mark.parametrize('profile', GOLDEN['profiles'] + GOLDEN.get('legacyProfiles', []),
                         ids=lambda p: p.get('family', 'classic1k'))
def test_frozen_profiles_and_reject_identity_credentials_duplicates_geometry(profile):
    values = hf.profile_blocks(profile)
    family = hf.profile_family(profile)
    assert set(values) == set(hf.eligible_units(family))
    for mutation in ('uid', 'key', 'duplicate', 'metadata', 'geometry', 'cross-family'):
        changed = copy.deepcopy(profile)
        if mutation in ('uid', 'key'):
            changed[mutation] = 'forbidden'
        elif mutation == 'duplicate':
            changed['blocks'][-1] = changed['blocks'][0]
        elif mutation == 'metadata':
            changed['blocks'][0]['number'] = 0
        elif mutation == 'geometry':
            if family == 'classic1k':
                changed['format'] = 'unknown'
            else:
                changed['geometry']['unitCount'] += 1
        with pytest.raises(CardError):
            hf.profile_blocks(changed, 'unknown' if mutation == 'cross-family' else family)
    assert json.loads(json.dumps(profile)) == profile


class Classic4K(Card):
    def __init__(self, formatted=False):
        super().__init__(False)
        self.info = hf.CardInfo(b'\1\2\3\4', atqa=b'\2\0', sak=0x18)
        self.memory = {b: bytes(16) for b in range(256)}
        for s in range(40):
            self.memory[hf.sector_blocks(s)[-1]] = bytes.fromhex('FFFFFFFFFFFFFF078069FFFFFFFFFFFF')
        self.formatted = False
        if formatted:
            mad = bytearray(b'\0\1' + b'\0\0' * 15)
            mad[0] = hf.mad_crc(mad[1:])
            self.memory[1], self.memory[2] = bytes(mad[:16]), bytes(mad[16:])
            self.memory[3] = bytes.fromhex('000000000000787788C2FFFFFFFFFFFF')
            mad2 = bytearray(48)
            mad2[2+(32-17)*2:4+(32-17)*2] = b'\3\xe1'
            mad2[2+(39-17)*2:4+(39-17)*2] = b'\3\xe1'
            mad2[0] = hf.mad_crc(mad2[1:])
            for i, b in enumerate((64, 65, 66)):
                self.memory[b] = bytes(mad2[i*16:i*16+16])
            self.memory[67] = bytes.fromhex('00000000000078778800FFFFFFFFFFFF')
            for s in (32, 39):
                self.memory[hf.sector_blocks(s)[-1]] = bytes.fromhex('0000000000007F078840FFFFFFFFFFFF')
            message = ndef.encode([ndef.Record.text('4K MAD2')])
            area = (bytes([3, len(message)])+message+b'\xfe').ljust(480, b'\0')
            for i, b in enumerate([*hf.sector_blocks(32)[:-1], *hf.sector_blocks(39)[:-1]]):
                self.memory[b] = area[i*16:i*16+16]


async def test_classic4k_full_read_data_write_and_profile_preserve_all_trailers():
    source, target = Classic4K(), Classic4K()
    for b in hf.eligible_units('classic4k'):
        source.memory[b] = bytes([b]) * 16
    client = hf.ClassicClient(source)
    snapshot = await client.read()
    assert len(snapshot.blocks) == 256 and set(snapshot.keys) == set(range(40))
    profile = hf.profile_from_snapshot(snapshot)
    assert profile['family'] == 'classic4k' and len(profile['blocks']) == 215
    original = dict(target.memory)
    result = await hf.ClassicClient(target).write_profile(profile)
    assert result == target.info and len(target.writes) == 215
    assert all(target.memory[b] == original[b] for b in range(256) if not hf.data_block(b, 256))
    assert target.memory[128] == bytes([128]) * 16 and target.memory[254] == bytes([254]) * 16
    for block in (0, 127, 143, 255, 256):
        with pytest.raises(CardError):
            await client.write_block(block, bytes(16), snapshot)


async def test_classic4k_mad2_directory_crc_allocation_and_length_last_write():
    card = Classic4K(True)
    client = hf.ClassicClient(card)
    area = hf.ClassicNDEF(await client.read())
    assert area.blocks == [*range(128, 143), *range(240, 255)]
    assert area.records[0].display()[1] == '4K MAD2'
    records = [ndef.Record.text('new MAD2 record ' * 20)]
    original = dict(card.memory)
    result = await client.write_records(records, area)
    assert hf.ClassicNDEF(result).records == records
    assert card.writes[0][0] == 128 and card.writes[0][1][1] == 0
    assert card.writes[-1][0] == 128 and card.writes[-1][1][1] == 255
    assert all(card.memory[b] == original[b] for b in (0, 1, 2, 3, 64, 65, 66, 67, 143, 255))
    card.memory[65] = b'bad directory!!!'
    with pytest.raises(CardError, match='MAD2'):
        hf.ClassicNDEF(await client.read())


async def test_cross_classic_family_profiles_rejected_without_mutation():
    profile = hf.profile_from_snapshot(await hf.ClassicClient(Card(False)).read())
    card = Classic4K()
    with pytest.raises(CardError, match='families'):
        await hf.ClassicClient(card).write_profile(profile)
    assert not card.writes


class NTAG:
    def __init__(self, family='ntag213'):
        self.info = hf.CardInfo(bytes.fromhex('04010203040506'), atqa=b'\x44\0', sak=0,
                                family_hint=family, version=bytes.fromhex('000404020100') +
                                bytes([{'ntag213': 15, 'ntag215': 17, 'ntag216': 19}[family], 3]))
        self.pages = {p: bytes(4) for p in range(self.info.unit_count)}
        self.pages[3] = bytes([0xe1, 0x10, {'ntag213': 18, 'ntag215': 62, 'ntag216': 109}[family], 0])
        self.pages[self.info.unit_count-4] = b'\0\0\0\xff'
        self.authentication = b'\0'
        self.writes = []
        self.reject = None
        message = ndef.encode([ndef.Record.text('NTAG')])
        area = (bytes([3, len(message)]) + message + b'\xfe').ljust(len(hf.eligible_units(family))*4, b'\0')
        for i, p in enumerate(hf.eligible_units(family)):
            self.pages[p] = area[i*4:i*4+4]

    async def identify(self):
        return self.info

    async def present(self):
        return True

    async def read_pages(self, page, count=1):
        return b''.join(self.pages[p] for p in range(page, page+count))

    async def write_page(self, page, value):
        self.writes.append((page, value))
        if page == self.reject:
            raise CardError('simulated interrupted physical write')
        self.pages[page] = value


@pytest.mark.parametrize('family,count', [('ntag213', 36), ('ntag215', 126), ('ntag216', 222)])
async def test_ntag_read_profile_and_ndef_capacity(family, count):
    card = NTAG(family)
    client = ntag.Client(card)
    snapshot = await client.read()
    assert len(snapshot.blocks) == card.info.unit_count - 2
    assert card.info.unit_count-2 not in snapshot.blocks
    area = ntag.Type2NDEF(snapshot)
    assert len(area.memory) == snapshot.blocks[3][2] * 8
    assert area.writable and area.records[0].display()[1] == 'NTAG'
    profile = hf.profile_from_snapshot(snapshot)
    assert profile['family'] == family and len(profile['blocks']) == count
    target = NTAG(family)
    target.pages[4] = b'xxxx'
    original = dict(target.pages)
    await ntag.Client(target).write_profile(profile)
    assert target.writes == [(4, card.pages[4])]
    assert all(target.pages[p] == original[p] for p in range(card.info.unit_count) if p not in hf.eligible_units(family))


@pytest.mark.parametrize('family', list(hf.NTAG_LAYOUTS))
@pytest.mark.parametrize('failure', ['static', 'dynamic', 'cc', 'auth', 'missing-locks'])
async def test_ntag_permission_preflight(family, failure):
    card = NTAG(family)
    snapshot = await ntag.Client(card).read()
    page = 4
    if failure == 'static':
        snapshot.blocks[2] = b'\0\0\x10\0'
    elif failure == 'dynamic':
        page = 16
        snapshot.blocks[card.info.unit_count-5] = b'\1\0\0\0'
    elif failure == 'cc':
        snapshot.blocks[3] = b'\xe1\x10\x12\x0f'
    elif failure == 'auth':
        snapshot.blocks[card.info.unit_count-4] = b'\0\0\0\4'
    else:
        del snapshot.blocks[card.info.unit_count-5]
    try:
        allowed = ntag.writable(snapshot, page)
    except CardError:
        allowed = False
    assert not allowed and not card.writes


@pytest.mark.parametrize('family', list(hf.NTAG_LAYOUTS))
async def test_ntag_boundary_locks_authentication_and_protected_metadata(family):
    card = NTAG(family)
    snapshot = await ntag.Client(card).read()
    last = card.info.unit_count-6
    snapshot.blocks[card.info.unit_count-4] = b'\0\0\0\4'
    assert not ntag.writable(snapshot, last)
    assert ntag.writable(snapshot, last, True)
    group = (last-16)//{'ntag213': 2, 'ntag215': 16, 'ntag216': 16}[family]
    snapshot.blocks[card.info.unit_count-5] = (1 << group).to_bytes(2, 'little')+b'\0\0'
    assert not ntag.writable(snapshot, last, True)
    for p in (0, 1, 2, 3, card.info.unit_count-5, card.info.unit_count-1):
        with pytest.raises(CardError, match='Protected'):
            ntag.writable(snapshot, p, True)


@pytest.mark.parametrize('family', list(hf.NTAG_LAYOUTS))
async def test_ntag_ndef_length_last_and_preserves_metadata(family):
    card = NTAG(family)
    client = ntag.Client(card)
    area = ntag.Type2NDEF(await client.read())
    records = [ndef.Record.text('changed ' * (40 if family != 'ntag213' else 10))]
    original = dict(card.pages)
    result = await client.write_records(records, area)
    assert ntag.Type2NDEF(result).records == records
    assert card.writes[0][0] == card.writes[-1][0] == 4
    assert card.writes[0][1][1] == 0 and card.writes[-1][1][1] != 0
    assert all(p in hf.eligible_units(family) for p, _ in card.writes)
    assert all(card.pages[p] == original[p] for p in range(card.info.unit_count) if p not in hf.eligible_units(family))


async def test_ntag_partial_failure_does_not_commit_length_or_retry():
    card = NTAG()
    client = ntag.Client(card)
    area = ntag.Type2NDEF(await client.read())
    card.reject = 5
    with pytest.raises(CardError, match='not verified'):
        await client.write_records([ndef.Record.text('longer message')], area)
    assert card.pages[4][1] == 0 and sum(p == 5 for p, _ in card.writes) == 1


async def test_ntag_profile_preflights_every_page_before_first_write():
    source, target = NTAG(), NTAG()
    profile = hf.profile_from_snapshot(await ntag.Client(source).read())
    target.pages[4] = b'xxxx'
    target.pages[40] = b'\0\x08\0\0'
    with pytest.raises(CardError, match='protected'):
        await ntag.Client(target).write_profile(profile)
    assert not target.writes


async def test_ntag215_dynamic_lock_bit_one_locks32_to47_not24_to31():
    snapshot = await ntag.Client(NTAG('ntag215')).read()
    snapshot.blocks[130] = b'\2\0\0\xbd'
    assert all(ntag.writable(snapshot, p) for p in range(24, 32))
    assert not any(ntag.writable(snapshot, p) for p in range(32, 48))
    assert ntag.writable(snapshot, 48)


@pytest.mark.parametrize('family', list(hf.NTAG_LAYOUTS))
async def test_block_locks_and_dynamic_byte3_are_not_page_locks(family):
    snapshot = await ntag.Client(NTAG(family)).read()
    snapshot.blocks[2] = b'\0\0\7\0'
    snapshot.blocks[snapshot.card.unit_count-5] = b'\0\0\xff\xbd'
    assert all(ntag.writable(snapshot, p) for p in hf.eligible_units(family))


@pytest.mark.parametrize('family,control', [('ntag213', 'a00c34'), ('ntag215', '880866'), ('ntag216', 'e80e66')])
async def test_known_external_lock_tlv_is_writable_and_preserved(family, control):
    card = NTAG(family)
    message = ndef.encode([ndef.Record.text('with Lock Control')])
    prefix = b'\1\3'+bytes.fromhex(control)
    area = (prefix + bytes([3, len(message)]) + message + b'\xfe').ljust(len(hf.eligible_units(family))*4, b'\0')
    for i, p in enumerate(hf.eligible_units(family)):
        card.pages[p] = area[i*4:i*4+4]
    client = ntag.Client(card)
    expected = ntag.Type2NDEF(await client.read())
    assert expected.writable and expected.prefix == prefix
    result = await client.write_records([ndef.Record.text('changed and preserved')], expected)
    assert ntag.Type2NDEF(result).prefix == prefix


async def test_unknown_control_mapping_and_oversized_cc_do_not_allow_writes():
    card = NTAG()
    snapshot = await ntag.Client(card).read()
    data = b'\1\3\0\x0c\x34' + b'\3\0\xfe'
    for i in range(2):
        snapshot.blocks[4+i] = data[i*4:i*4+4]
    assert not ntag.Type2NDEF(snapshot).writable
    snapshot.blocks[3] = b'\xe1\x10\xff\0'
    with pytest.raises(CardError):
        ntag.Type2NDEF(snapshot)
    with pytest.raises(CardError):
        ntag.writable(snapshot, 4)


@pytest.mark.parametrize('case', GOLDEN['ntagLockCases'], ids=lambda c: c['name'])
async def test_shared_ntag_lock_vectors(case):
    snapshot = await ntag.Client(NTAG(case['family'])).read()
    snapshot.blocks[2] = bytes(2)+bytes.fromhex(case['staticLockHex'])
    snapshot.blocks[snapshot.card.unit_count-5] = bytes.fromhex(case['dynamicLockPageHex'])
    locked = [p for p in hf.eligible_units(case['family']) if not ntag.writable(snapshot, p)]
    assert locked == case['lockedUserPages']


@pytest.mark.parametrize('case', GOLDEN['ntagLockControlCases'], ids=lambda c: c['family'])
async def test_shared_ntag_control_vectors(case):
    area = ntag.Type2NDEF(await ntag.Client(NTAG(case['family'])).read())
    assert area.control_tlv(1, bytes.fromhex(case['lockControlHex']))
    assert case['lockByteAddress'] == (area.card.unit_count-5)*4


async def test_shared_standard_mad2_fixture_read_and_write_across_large_sectors():
    fixture = GOLDEN['classic4kNdef']
    card = Classic4K()
    card.memory = {b['number']: bytes.fromhex(b['hex']) for b in fixture['blocks']}
    client = hf.ClassicClient(card)
    snapshot = await client.read()
    area = hf.ClassicNDEF(snapshot)
    assert area.blocks == fixture['ndefBlocks']
    assert all(b not in area.blocks for b in (0, 1, 2, 3, 64, 65, 66, 67, 143, 255))
    assert ndef.encode(area.records).hex() == fixture['messageHex']
    original = dict(card.memory)
    records = [ndef.Record.text('M' * 1500)]
    result = await client.write_records(records, area)
    assert hf.ClassicNDEF(result).records == records
    assert any(b >= 128 for b, _ in card.writes)
    assert all(card.memory[b] == original[b] for b in range(256) if b not in area.blocks)


async def test_mad2_standard_key_for_sector16_and_reserved_gpb_validation():
    from quadband.common import AuthenticationError
    card = Classic4K(True)
    original_read = card.read_block
    async def read(block, credential):
        if hf.sector_of(block) == 16:
            if credential != (bytes.fromhex('A0A1A2A3A4A5'), 0x60):
                raise AuthenticationError('MAD2 key required')
            return card.memory[block]
        return await original_read(block, credential)
    card.read_block = read
    snapshot = await hf.ClassicClient(card).read()
    assert snapshot.keys[16] == (bytes.fromhex('A0A1A2A3A4A5'), 0x60)
    assert hf.ClassicNDEF(snapshot).blocks
    snapshot.blocks[67] = snapshot.blocks[67][:9] + b'\xc2' + snapshot.blocks[67][10:]
    with pytest.raises(CardError, match='MAD2'):
        hf.ClassicNDEF(snapshot)


def make_access(codes):
    trailer = bytearray(16)
    for i, code in enumerate(codes):
        c1, c2, c3 = code >> 2 & 1, code >> 1 & 1, code & 1
        trailer[6] |= ((1-c2) << (4+i)) | ((1-c1) << i)
        trailer[7] |= (c1 << (4+i)) | ((1-c3) << i)
        trailer[8] |= (c3 << (4+i)) | (c2 << i)
    return bytes(trailer)


def test_large_sector_access_groups_and_key_b_authentication_rules():
    card = hf.CardInfo(b'1234', atqa=b'\2\0', sak=0x18)
    snapshot = hf.Snapshot(card, blocks={143: make_access((0, 3, 2, 3))}, keys={32: hf.FACTORY})
    assert all(hf.writable(snapshot, b) for b in range(128, 133))
    assert not any(hf.writable(snapshot, b) for b in range(133, 143))
    snapshot.keys[32] = (bytes(6), 0x61)
    assert all(hf.writable(snapshot, b) for b in range(128, 138))
    assert not any(hf.writable(snapshot, b) for b in range(138, 143))
    snapshot.blocks[143] = make_access((0, 0, 0, 1))
    assert not hf.writable(snapshot, 128)
    snapshot.keys[32] = hf.FACTORY
    corrupted = bytearray(snapshot.blocks[143])
    corrupted[6] ^= 8
    snapshot.blocks[143] = bytes(corrupted)
    with pytest.raises(CardError, match='access'):
        hf.writable(snapshot, 128)


@pytest.mark.parametrize('family', list(hf.NTAG_LAYOUTS))
async def test_initialized_all_null_ntag_can_receive_first_ndef_length_last(family):
    card = NTAG(family)
    for page in hf.eligible_units(family):
        card.pages[page] = bytes(4)
    client = ntag.Client(card)
    original = dict(card.pages)
    snapshot = await client.read()
    area = ntag.Type2NDEF(snapshot)
    assert area.records == [] and area.size == 0 and area.writable
    assert area.memory == bytes(card.pages[3][2]*8)
    records = [ndef.Record.text('First NFC message')]
    result = await client.write_records(records, area)
    assert ntag.Type2NDEF(result).records == records
    assert card.writes[0] == (4, bytes(4))
    assert card.writes[-1][0] == 4 and card.writes[-1][1][1] != 0
    assert all(p in hf.eligible_units(family) for p, _ in card.writes)
    assert all(card.pages[p] == original[p] for p in range(card.info.unit_count)
               if p not in hf.eligible_units(family))


@pytest.mark.parametrize('failure', ['unknown-data', 'stale-preview', 'locked', 'invalid-cc'])
async def test_first_ntag_ndef_does_not_overwrite_unknown_stale_or_protected_data(failure):
    card = NTAG()
    for page in hf.eligible_units(card.info.family):
        card.pages[page] = bytes(4)
    client = ntag.Client(card)
    expected = ntag.Type2NDEF(await client.read())
    if failure in ('unknown-data', 'stale-preview'):
        card.pages[5] = b'ABCD'
    elif failure == 'locked':
        card.pages[2] = b'\0\0\x10\0'
    else:
        card.pages[3] = b'\xe1\x10\xff\0'
    with pytest.raises(CardError):
        await client.write_records([ndef.Record.text('Must not write')], expected)
    assert not card.writes


async def test_empty_ntag_partial_first_message_never_commits_or_retries():
    card = NTAG()
    for page in hf.eligible_units(card.info.family):
        card.pages[page] = bytes(4)
    client = ntag.Client(card)
    expected = ntag.Type2NDEF(await client.read())
    card.reject = 5
    with pytest.raises(CardError, match='not verified'):
        await client.write_records([ndef.Record.text('First message interrupted')], expected)
    assert card.pages[4][1] == 0 and sum(p == 5 for p, _ in card.writes) == 1
