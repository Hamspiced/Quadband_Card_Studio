import asyncio
from contextlib import asynccontextmanager
import copy
import pytest
from quadband import hf, ndef
from quadband.common import CardError, AuthenticationError
from quadband.service import StopFlag


class Card:
    def __init__(self, formatted=True):
        self.info = hf.CardInfo(b'\1\2\3\4', bytes.fromhex('3B8F8001804F0CA000000306030001000000006A'))
        self.memory = {b: bytes(16) for b in range(64)}
        self.writes, self.resets, self.halted, self.reject, self.bad_verify = [], 0, False, None, False
        self.formatted = formatted
        for s in range(16):
            self.memory[s*4+3] = bytes.fromhex('FFFFFFFFFFFFFF078069FFFFFFFFFFFF')
        if formatted:
            mad = bytearray(b'\0\1'+b'\3\xe1'*15)
            mad[0] = hf.mad_crc(mad[1:])
            self.memory[1], self.memory[2] = bytes(mad[:16]), bytes(mad[16:])
            self.memory[3] = bytes.fromhex('000000000000787788C1FFFFFFFFFFFF')
            for s in range(1, 16):
                self.memory[s*4+3] = bytes.fromhex('0000000000007F078840FFFFFFFFFFFF')
            message = ndef.encode([ndef.Record.uri('https://midwestgadgets.org')])
            area = (bytes([3, len(message)])+message+b'\xfe').ljust(720, b'\0')
            for i, b in enumerate(b for b in range(4, 64) if hf.data_block(b)):
                self.memory[b] = area[i*16:i*16+16]

    async def identify(self):
        return self.info

    async def present(self):
        return True

    async def read_block(self, block, credential):
        correct = (bytes.fromhex('A0A1A2A3A4A5' if block < 4 else 'D3F7D3F7D3F7'), 0x60) if self.formatted else hf.FACTORY
        if credential != correct or self.halted:
            self.halted = True
            raise AuthenticationError('Wrong key')
        return bytes(16) if self.bad_verify and self.writes and block == self.writes[-1][0] else self.memory[block]

    async def write_block(self, block, data, credential):
        self.writes.append((block, data))
        if block == self.reject:
            raise CardError('Simulated write failure')
        self.memory[block] = data

    async def reset_authentication(self):
        self.halted = False
        self.resets += 1


async def test_automatic_keys_recover_halted_card_and_read_url():
    card = Card()
    snapshot = await hf.ClassicClient(card).read((bytes(6), 0x60))
    area = hf.ClassicNDEF(snapshot)
    assert card.resets >= 16 and len(snapshot.blocks) == 64 and not snapshot.errors
    assert area.records[0].display()[1] == 'https://midwestgadgets.org'
    assert area.size == 23 and area.capacity == 716 and area.writable


async def test_ndef_staged_commit_preserves_directory_and_keys():
    card, records = Card(), [ndef.Record.text('Changed '*45), ndef.Record.uri('https://example.org')]
    client = hf.ClassicClient(card)
    expected = hf.ClassicNDEF(await client.read())
    original = dict(card.memory)
    result = await client.write_records(records, expected)
    assert hf.ClassicNDEF(result).records == records
    assert card.writes[0][0] == 4 and card.writes[0][1][1] == 0
    assert card.writes[-1][0] == 4 and card.writes[-1][1][1] == 255
    assert all(b >= 4 and hf.data_block(b) for b, _ in card.writes)
    assert all(card.memory[b] == original[b] for b in [0, 1, 2]+list(range(3, 64, 4)))


@pytest.mark.parametrize('scenario', ['uid', 'stale', 'large', 'read-only', 'access-complement'])
async def test_ndef_preflight_rejects_without_writing(scenario):
    card = Card()
    if scenario == 'read-only':
        card.memory[7] = card.memory[7][:9]+b'\x43'+card.memory[7][10:]
    client = hf.ClassicClient(card)
    expected = hf.ClassicNDEF(await client.read())
    if scenario == 'uid':
        card.info = hf.CardInfo(bytes([5,6,7,8]), card.info.atr)
    elif scenario == 'stale':
        card.memory[5] = b'\1'+card.memory[5][1:]
    elif scenario == 'access-complement':
        card.memory[7] = card.memory[7][:6]+bytes(3)+card.memory[7][9:]
    with pytest.raises(CardError):
        await client.write_records([ndef.Record.text('x'*(800 if scenario == 'large' else 5))], expected)
    assert not card.writes


async def test_partial_ndef_failure_leaves_empty_length_and_never_retries():
    card = Card()
    client = hf.ClassicClient(card)
    expected = hf.ClassicNDEF(await client.read())
    card.reject = 5
    with pytest.raises(CardError, match='not verified'):
        await client.write_records([ndef.Record.text('changed'*30)], expected)
    assert card.memory[4][1] == 0
    assert sum(b == 5 for b, _ in card.writes) == 1


async def test_profile_cross_platform_shape_excludes_uid_keys_trailers():
    profile = hf.profile_from_snapshot(await hf.ClassicClient(Card()).read())
    assert len(hf.profile_blocks(profile)) == 47
    assert set(profile) == {'format', 'version', 'name', 'createdAt', 'blocks'}
    for bad in [0, 3, 63, 1]:
        modified = copy.deepcopy(profile)
        modified['blocks'][-1]['number'] = bad
        with pytest.raises(CardError):
            hf.profile_blocks(modified)


async def test_copy_preserves_uid_keys_and_verifies_all_data():
    source, target = Card(False), Card(False)
    for b in hf.DATA_BLOCKS:
        source.memory[b] = bytes([b])*16
    profile = hf.profile_from_snapshot(await hf.ClassicClient(source).read())
    original = dict(target.memory)
    result = await hf.ClassicClient(target).write_profile(profile)
    assert result == target.info and len(target.writes) == 47
    assert all(target.memory[b] == original[b] for b in range(64) if not hf.data_block(b))
    assert all(target.memory[b] == source.memory[b] for b in hf.DATA_BLOCKS)


async def test_bad_write_readback_is_failure_no_retry():
    card = Card(False)
    client = hf.ClassicClient(card)
    snapshot = await client.read()
    card.bad_verify = True
    with pytest.raises(CardError, match='not verified'):
        await client.write_block(1, b'\xaa'*16, snapshot)
    assert len(card.writes) == 1


async def test_copy_continues_after_failure_and_requires_actual_removal():
    profile = hf.profile_from_snapshot(await hf.ClassicClient(Card(False)).read())
    stop, results, writes = StopFlag(), [], []
    # Held source, removal, failed target, held failed target, removal, successful target.
    presentations = iter([True, False, True, True, False, True])
    class BatchClient:
        backend = None
        async def present(self):
            return next(presentations)
        async def write_profile(self, value, key):
            writes.append(1)
            if len(writes) == 1:
                raise CardError('Target rejected data')
            return hf.CardInfo(b'\1\2\3\4')  # Same UID is intentionally allowed after removal.
    client = BatchClient()
    client.backend = client
    @asynccontextmanager
    async def factory():
        yield client
    def result(success, detail):
        results.append(success)
        if len(results) == 2:
            stop.set()
    await asyncio.wait_for(hf.copy_forever(factory, profile, hf.FACTORY, stop, lambda _: None, result, .001), 1)
    assert results == [False, True] and len(writes) == 2


async def test_disconnect_is_not_card_removal():
    profile = hf.profile_from_snapshot(await hf.ClassicClient(Card(False)).read())
    stop, writes = StopFlag(), []
    states = iter([False, 'disconnect', True, True, False, True])
    class Client:
        backend = None
        async def present(self):
            value = next(states)
            if value == 'disconnect':
                raise CardError('Disconnected')
            return value
        async def write_profile(self, *_):
            writes.append(1)
            stop.set()
            return hf.CardInfo(b'\1\2\3\4')
    client = Client()
    client.backend = client
    @asynccontextmanager
    async def factory():
        yield client
    await asyncio.wait_for(hf.copy_forever(factory, profile, hf.FACTORY, stop, lambda _: None, lambda *_: None, .001), 1)
    assert len(writes) == 1


async def test_copy_formatted_card_does_not_require_writing_unchanged_mad():
    source, target = Card(), Card()
    target.memory[5] = b'Z'*16
    profile = hf.profile_from_snapshot(await hf.ClassicClient(source).read())
    await hf.ClassicClient(target).write_profile(profile)
    assert target.writes == [(5, source.memory[5])]


RECORDS = [ndef.Record.uri('https://midwestgadgets.org')]


def encoding_backend(card):
    from test_type2 import Physical
    backend = Physical(card)
    if card.formatted:
        backend.keys = {s: (bytes.fromhex('A0A1A2A3A4A5' if s == 0 else 'D3F7D3F7D3F7'), 0x60)
                        for s in range(16)}
    return backend


async def encode_card(card, records=RECORDS, overwrite=False, format=True, backend=None):
    from quadband import bulk, cores3
    backend = backend or encoding_backend(card)
    client = cores3.HFClient(backend)
    job = bulk.Job.freeze('classic1k', records, overwrite=overwrite, format=format)
    review = await bulk.prepare(client, job)
    already = review.area is not None and review.area.records == list(job.records)
    snapshot, _ = await bulk.execute(client, review)
    return snapshot, review.needs_format, not already, backend


async def test_encode_formats_blank_tag_then_writes_and_verifies():
    card = Card(False)
    snapshot, formatted, written, _ = await encode_card(card)
    assert formatted and written and hf.ClassicNDEF(snapshot).records == RECORDS
    trailers = [b for b, _ in card.writes if b % 4 == 3]
    assert trailers[-1] == 3 and sorted(trailers) == list(range(3, 64, 4))
    assert card.memory[0] == bytes(16)


async def test_encode_replaces_formatted_tag_and_skips_identical():
    card = Card()
    backend = encoding_backend(card)
    _, formatted, written, _ = await encode_card(card, [ndef.Record.text('new')], overwrite=True, backend=backend)
    assert not formatted and written
    card.writes.clear()
    _, _, written, _ = await encode_card(card, [ndef.Record.text('new')], backend=backend)
    assert not written and not card.writes


async def test_format_refuses_tags_holding_data_or_locked_keys():
    card = Card(False)
    card.memory[5] = b'\x55'*16
    with pytest.raises(CardError, match='already holds data'):
        await encode_card(card)
    assert not card.writes
    _, formatted, _, _ = await encode_card(card, overwrite=True)
    assert formatted
    locked = Card(False)
    locked.memory[7] = locked.memory[7][:6]+bytes.fromhex('787788')+locked.memory[7][9:]
    with pytest.raises(CardError, match='factory'):
        await encode_card(locked)
    assert not locked.writes


async def test_format_failure_is_reported_and_does_not_continue():
    card = Card(False)
    backend = encoding_backend(card)
    backend.reject = 4
    with pytest.raises(CardError, match='unverified'):
        await encode_card(card, backend=backend)
    assert len(card.writes) == 1 and card.writes[0][0] == 4


async def drive_encoding(steps, confirm_hook=None):
    from quadband import bulk, cores3
    stop, results, phases, reviews = StopFlag(), [], [], []
    class Reader:
        def __init__(self):
            self.steps, self.backends, self.current = list(steps), {}, None
        def __getattr__(self, name):
            if self.current is None:
                raise AttributeError(name)
            return getattr(self.current, name)
        async def present(self):
            if not self.steps:
                stop.set()
                return False
            card = self.steps.pop(0)
            if isinstance(card, Exception):
                raise card
            if card is None:
                self.current = None
                return False
            if card not in self.backends:
                self.backends[card] = encoding_backend(card)
            self.current = self.backends[card]
            self.current.cancelled = stop.is_set
            return True
    reader = Reader()
    @asynccontextmanager
    async def factory():
        yield cores3.HFClient(reader)
    async def confirm(review):
        reviews.append(review)
        if confirm_hook:
            confirm_hook(reader)
        return True
    job = bulk.Job.freeze('classic1k', RECORDS, format=True)
    await asyncio.wait_for(bulk.run(factory, job, hf.FACTORY, stop, phases.append,
                                   lambda *r: results.append(r), confirm, .001), 5)
    return results, phases, reviews


async def test_bulk_encodes_each_tag_once_and_requires_removal():
    a, b = Card(False), Card()
    b.info = hf.CardInfo(b'\2\2\2\2', b.info.atr)
    results, phases, reviews = await drive_encoding([None, a, a, a, None, b, b, None, a])
    assert [r[0] for r in results] == [True, True, True]
    assert 'Formatted' in results[0][1] and 'Already correct' in results[1][1]
    assert 'Already correct' in results[2][1] and len(reviews) == 3
    assert not b.writes
    assert any('Remove' in phase for phase in phases)


async def test_bulk_failure_counts_continues_and_multiple_tag_trouble_is_not_counted():
    bad, good = Card(False), Card(False)
    bad.memory[5] = b'\x55'*16
    good.info = hf.CardInfo(b'\2\2\2\2', good.info.atr)
    results, phases, _ = await drive_encoding([None, bad, None, CardError('Keep one HF card on the antenna.'), None, good])
    assert [r[0] for r in results] == [False, True] and 'already holds data' in results[0][1]
    assert not bad.writes and good.writes
    assert any('Reader error is not removal' in phase for phase in phases)


async def test_bulk_does_not_write_when_identity_changes_after_review():
    card = Card(False)
    def swap(reader):
        reader.current.tag.info = hf.CardInfo(b'\t'*4, card.info.atr)
    results, _, reviews = await drive_encoding([None, card], swap)
    assert len(reviews) == 1 and [r[0] for r in results] == [False]
    assert not card.writes
