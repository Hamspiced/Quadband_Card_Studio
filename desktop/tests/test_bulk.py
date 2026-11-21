import asyncio
from contextlib import asynccontextmanager
from dataclasses import replace
import pytest
from quadband import bulk, cores3, formatting, hf, ndef, ntag, pcsc
from quadband.common import CardError, AuthenticationError
from quadband.service import StopFlag
from test_hf_families import NTAG
from test_hf import Card
from test_type2 import Physical, RECORDS


@pytest.mark.parametrize('gpb', [0xc1, 0xc2])
@pytest.mark.parametrize('overwrite', [False, True])
async def test_factory_keys_and_access_do_not_authorize_partial_mad_repair(gpb, overwrite):
    card = Card(False)
    trailer = bytearray(card.memory[3])
    trailer[9] = gpb
    card.memory[3] = bytes(trailer)
    backend = Physical(card)
    snapshot = await hf.ClassicClient(backend).read(hf.FACTORY, automatic=False)
    with pytest.raises(CardError, match='partial MAD'):
        await backend.canonical_format(snapshot, overwrite)
    assert not card.writes


@pytest.mark.parametrize('family', hf.NTAG_LAYOUTS)
async def test_canonical_ntag_accepts_reserved_dynamic_byte_bd_and_preserves_it(family):
    tag = NTAG(family)
    tag.pages[3] = bytes(4)
    for p in hf.eligible_units(family):
        tag.pages[p] = bytes(4)
    dynamic = tag.info.unit_count-5
    tag.pages[dynamic] = b'\0\0\0\xbd'
    backend = Physical(tag)
    snapshot = await ntag.Client(backend).read()
    await backend.canonical_format(snapshot)
    assert tag.pages[dynamic] == b'\0\0\0\xbd'
    assert all(p != dynamic for p, _ in tag.writes)


@pytest.mark.parametrize('family', ['classic1k', 'ntag215'])
async def test_formatting_rechecks_permissions_before_every_mutation(family):
    tag = Card(False) if family == 'classic1k' else NTAG(family)
    if tag.info.ntag:
        tag.pages[3] = bytes(4)
    backend = Physical(tag)
    snapshot = await (ntag.Client(backend).read() if tag.info.ntag
                      else hf.ClassicClient(backend).read(hf.FACTORY, automatic=False))
    original = backend._format_unit
    async def changed_permissions(unit, data, credential):
        await original(unit, data, credential)
        if tag.info.ntag:
            tag.pages[2] = b'\0\0\x10\0'
        else:
            tag.memory[7] = bytes(16)
    backend._format_unit = changed_permissions
    with pytest.raises(CardError, match='unverified'):
        await backend.canonical_format(snapshot, True)
    assert len(tag.writes) == 1


@pytest.mark.parametrize('gpb', [0x00, 0x69, 0xff])
async def test_factory_preflight_accepts_non_mad_gpb_without_requiring_69(gpb):
    card = Card(False)
    for trailer in range(3, 64, 4):
        data = bytearray(card.memory[trailer])
        data[9] = gpb
        card.memory[trailer] = bytes(data)
    snapshot = await hf.ClassicClient(Physical(card)).read(hf.FACTORY, automatic=False)
    formatting.preflight(snapshot)
    assert not card.writes


async def test_prefix_free_ntag213_exact142_draft_fits_but_factory_controls_reduce_actual_fit():
    record = ndef.Record(2, b'x', b'z'*138)
    job = bulk.Job.freeze('ntag213', [record], overwrite=True)
    assert len(ndef.encode(job.records)) == 142
    card = NTAG('ntag213')
    backend = Physical(card)
    client = cores3.HFClient(backend)
    review = await bulk.prepare(client, job)
    assert review.area.capacity == 142
    snapshot, _ = await bulk.execute(client, review)
    assert ntag.Type2NDEF(snapshot).records == [record]
    card = NTAG('ntag213')
    image = (b'\1\3\xa0\x0c\x34\3\0\xfe').ljust(144, b'\0')
    for i in range(36):
        card.pages[4+i] = image[i*4:i*4+4]
    with pytest.raises(CardError, match='137-byte'):
        await bulk.prepare(cores3.HFClient(Physical(card)), job)
    assert not card.writes


@pytest.mark.parametrize('count', [0, 1, 3, 4])
def test_frozen_bulk_limit_does_not_limit_single_editor(count):
    records = [ndef.Record.text(str(i)) for i in range(count)]
    if count in (1, 3):
        job = bulk.Job.freeze('ntag213', records)
        assert not job.format and not job.overwrite
        records.reverse()
        assert job.records == tuple(ndef.Record.text(str(i)) for i in range(count))
    else:
        with pytest.raises(CardError, match='one to three'):
            bulk.Job.freeze('ntag213', records)
    if count:
        assert len(bulk.Job.freeze('ntag213', records, bulk=False).records) == count
        assert ndef.decode(ndef.encode(records)) == records


@pytest.mark.parametrize('family,prefix', [('ntag213', b'\1\3\xa0\x0c\x34'), ('ntag215', b''), ('ntag216', b'')])
@pytest.mark.parametrize('delta', [0, 1])
async def test_utf8_exact_fit_and_one_byte_overflow_use_actual_cc_and_tlv(family, prefix, delta):
    card = NTAG(family)
    cc_area = card.pages[3][2]*8
    room = cc_area-len(prefix)
    capacity = max(min(254, room-2), room-4)
    # Binary payload gives exact encoded sizes; the separate UTF-8 assertion counts encoded bytes.
    size = capacity+delta
    record = ndef.Record(2, b'x', b'z'*(size-(4 if size-4 < 256 else 7)))
    assert len(ndef.encode([record])) == size
    image = (prefix+b'\3\0\xfe').ljust(cc_area, b'\0')
    for i in range(cc_area//4):
        card.pages[4+i] = image[i*4:i*4+4]
    client = ntag.Client(card)
    area = ntag.Type2NDEF(await client.read())
    assert area.capacity == capacity
    if delta:
        with pytest.raises(CardError, match='exceed'):
            area.replacing([record])
    else:
        final = area.replacing([record])
        header = len(prefix)+2+(2 if size >= 255 else 0)
        assert len(final) == cc_area and final[len(prefix)+1] == (255 if size >= 255 else size)
        assert ndef.decode(final[header:]) == [record]
    unicode_record = ndef.Record.text('é😀')
    assert len(ndef.encode([unicode_record])) == 13


async def test_review_is_single_use_and_rejects_changed_bytes_credentials_and_family():
    for mutation in ('data', 'lock', 'identity', 'credential', 'reader'):
        card = NTAG('ntag215')
        backend = Physical(card)
        client = cores3.HFClient(backend)
        review = await bulk.prepare(client, bulk.Job.freeze('ntag215', RECORDS, True))
        if mutation == 'data':
            card.pages[20] = b'late'
        elif mutation == 'lock':
            card.pages[130] = b'\1\0\0\0'
        elif mutation == 'identity':
            card.info = replace(card.info, uid=b'7654321')
        elif mutation == 'credential':
            # A credential belongs to the reviewed connection, not just the UID.
            review.snapshot.keys[0] = hf.FACTORY
        else:
            backend.name = 'replacement'
        with pytest.raises(CardError):
            await bulk.execute(client, review)
        with pytest.raises(CardError, match='consumed'):
            await bulk.execute(client, review)
        assert not card.writes


async def test_empty_ndef_with_hidden_nonzero_tail_requires_overwrite_and_stale_keys_invalidate():
    tag = NTAG('ntag215')
    for page in hf.eligible_units(tag.info.family):
        tag.pages[page] = bytes(4)
    tag.pages[4] = b'\3\0\xfe\0'
    tag.pages[50] = b'KEEP'
    client = cores3.HFClient(Physical(tag))
    with pytest.raises(CardError, match='overwrite'):
        await bulk.prepare(client, bulk.Job.freeze('ntag215', RECORDS))
    assert not tag.writes
    review = await bulk.prepare(client, bulk.Job.freeze('ntag215', RECORDS, True))
    with pytest.raises(CardError, match='Credentials changed'):
        await bulk.execute(client, review, (bytes(6), 0x60))
    assert not tag.writes


async def drive(steps, accepted=True, fail=False):
    stop = StopFlag()
    results, reviews, phases = [], [], []
    tag = NTAG('ntag215')
    physical = Physical(tag)
    original_present = list(steps)
    @asynccontextmanager
    async def factory():
        async def present():
            if not original_present:
                stop.set()
                return True
            value = original_present.pop(0)
            if isinstance(value, Exception):
                raise value
            if value == 'swap':
                tag.info = replace(tag.info, uid=b'7654321')
                return True
            return value
        physical.present = present
        if fail:
            physical.reject = 5
        yield cores3.HFClient(physical)
    async def confirm(review):
        reviews.append(review)
        assert not tag.writes or len(reviews) > 1
        return accepted
    await asyncio.wait_for(bulk.run(factory, bulk.Job.freeze('ntag215', RECORDS, True), hf.FACTORY,
                                   stop, phases.append, lambda *v: results.append(v), confirm, .001), 3)
    return tag, reviews, results, phases


async def test_polling_alone_and_initial_held_target_never_write():
    tag, reviews, results, _ = await drive([True, True, 'swap', True])
    assert not tag.writes and not reviews and not results
    tag, reviews, results, _ = await drive([False, True, True, False, True], accepted=False)
    assert len(reviews) == 2 and not tag.writes and not results
    assert all(r.used for r in reviews)


async def test_bulk_initial_and_subsequent_observed_absence_even_different_uid():
    tag, reviews, results, _ = await drive([True, False, True, True, 'swap', True, False, True])
    assert len(reviews) == 2 and len(results) == 2 and all(r[0] for r in results)
    assert reviews[0].snapshot.card.uid != reviews[1].snapshot.card.uid
    assert ntag.Type2NDEF(await ntag.Client(tag).read()).records == RECORDS


@pytest.mark.parametrize('error', [CardError('reader disconnected'), AuthenticationError('unresponsive/auth')])
async def test_reader_error_does_not_arm_or_preserve_previous_absence(error):
    tag, reviews, results, phases = await drive([False, error, True, True, False, True])
    assert len(reviews) == 1 and len(results) == 1
    assert any('not removal' in p for p in phases)
    assert tag.writes


async def test_unverified_target_latches_job_stopped_and_never_retries():
    tag, reviews, results, phases = await drive([False, True, False, True], fail=True)
    assert len(reviews) == len(results) == 1 and not results[0][0]
    assert sum(p == 5 for p, _ in tag.writes) == 1
    assert 'Stopped' in phases[-1] and tag.pages[4][1] == 0


@pytest.mark.parametrize('when', ['before-confirm', 'during-confirm'])
async def test_stop_during_review_has_no_write(when):
    tag = NTAG('ntag215')
    backend = Physical(tag)
    stop = StopFlag()
    states = iter([False, True])
    @asynccontextmanager
    async def factory():
        async def present():
            value = next(states)
            if value and when == 'before-confirm':
                stop.set()
            return value
        backend.present = present
        yield cores3.HFClient(backend)
    async def confirm(_):
        stop.set()
        return True
    await bulk.run(factory, bulk.Job.freeze('ntag215', RECORDS, True), hf.FACTORY, stop,
                   lambda _: None, lambda *_: None, confirm, .001)
    assert not tag.writes


@pytest.mark.parametrize('family', ['classic1k', 'ntag215'])
async def test_failure_at_every_canonical_mutation_is_unverified_no_retry(family):
    def fresh():
        if family == 'classic1k':
            return Card(False)
        tag = NTAG(family)
        tag.pages[3] = bytes(4)
        return tag
    tag = fresh()
    backend = Physical(tag)
    snapshot = await (ntag.Client(backend).read() if tag.info.ntag
                      else hf.ClassicClient(backend).read(hf.FACTORY, automatic=False))
    await backend.canonical_format(snapshot, overwrite=True)
    units = [p for p, _ in tag.writes]
    assert units
    for fail_at in range(len(units)):
        tag = fresh()
        backend = Physical(tag)
        snapshot = await (ntag.Client(backend).read() if tag.info.ntag
                          else hf.ClassicClient(backend).read(hf.FACTORY, automatic=False))
        original = backend._format_unit
        attempts = []
        async def failing(unit, value, credential):
            attempts.append(unit)
            if len(attempts)-1 == fail_at:
                raise CardError('Injected mutation failure')
            await original(unit, value, credential)
        backend._format_unit = failing
        with pytest.raises(CardError, match='unverified'):
            await backend.canonical_format(snapshot, overwrite=True)
        assert len(attempts) == fail_at+1
        assert attempts == units[:fail_at+1]


async def test_cancel_after_first_format_mutation_is_unverified_and_no_more_writes():
    tag = NTAG('ntag215')
    tag.pages[3] = bytes(4)
    backend = Physical(tag)
    snapshot = await ntag.Client(backend).read()
    backend.cancelled = lambda: bool(tag.writes)
    with pytest.raises(CardError, match='unverified'):
        await backend.canonical_format(snapshot, True)
    assert len(tag.writes) == 1 and tag.pages[3] == bytes(4)


async def test_wrong_family_rejected_then_next_target_requires_fresh_absence_and_review():
    wrong, good = NTAG('ntag216'), NTAG('ntag215')
    backend, stop = Physical(wrong), StopFlag()
    states = iter([None, wrong, wrong, good, None, good])
    results, reviews = [], []
    async def present():
        value = next(states, 'done')
        if value == 'done':
            stop.set()
            return True
        if value is None:
            return False
        backend.tag = value
        return True
    backend.present = present
    @asynccontextmanager
    async def factory():
        yield cores3.HFClient(backend)
    async def confirm(review):
        reviews.append(review)
        return True
    await bulk.run(factory, bulk.Job.freeze('ntag215', RECORDS, True), hf.FACTORY, stop,
                   lambda _: None, lambda *args: results.append(args), confirm, .001)
    assert [r[0] for r in results] == [False, True]
    assert len(reviews) == 1 and not wrong.writes and good.writes


async def test_post_format_read_failure_is_unverified_and_review_cannot_be_reused():
    tag = NTAG('ntag213')
    tag.pages[3] = bytes(4)
    backend = Physical(tag)
    client = cores3.HFClient(backend)
    review = await bulk.prepare(client, bulk.Job.freeze('ntag213', RECORDS, True, True))
    original = backend.canonical_format
    async def formatted_then_lost(snapshot, overwrite):
        await original(snapshot, overwrite)
        async def lost():
            raise CardError('Reader lost after successful CC initialization')
        backend.identify = lost
    backend.canonical_format = formatted_then_lost
    with pytest.raises(CardError, match='Write unverified'):
        await bulk.execute(client, review)
    assert tag.writes and tag.pages[3] != bytes(4)
    with pytest.raises(CardError, match='consumed'):
        await bulk.execute(client, review)


@pytest.mark.parametrize('family', ['classic1k', 'ntag213'])
async def test_private_sink_cannot_be_used_for_arbitrary_protected_writes(family):
    card = Card(False) if family == 'classic1k' else NTAG(family)
    backend = Physical(card)
    await backend.identify()
    for unit in (0, 2, 3, 40, 43):
        with pytest.raises(CardError):
            await pcsc.PCSC._format_unit(backend, unit, b'BAD!', None)
    assert not card.writes


async def test_classic_read_authenticates_once_per_sector_but_invalidates_on_identity_and_reset():
    backend = pcsc.PCSC('fake')
    auths, reads = [], []
    async def auth(block, credential):
        auths.append((block, credential))
    async def command(command, count=None):
        reads.append(command[3])
        return bytes(16)
    backend.authenticate = auth
    backend.command = command
    for block in range(64):
        await backend.read_block(block, hf.FACTORY)
    assert len(auths) == 16 and len(reads) == 64
    backend.read_auth = None  # identify and reset both invalidate this token.
    await backend.read_block(63, hf.FACTORY)
    assert len(auths) == 17
