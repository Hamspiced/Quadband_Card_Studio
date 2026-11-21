"""Transient, target-bound NDEF jobs. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import copy
from dataclasses import dataclass
from . import hf, ntag, type2, ndef, formatting
from .common import CardError, require


@dataclass(frozen=True)
class Job:
    family: str
    records: tuple
    overwrite: bool = False
    format: bool = False

    @classmethod
    def freeze(cls, family, records, overwrite=False, format=False, bulk=True):
        require(not bulk or 1 <= len(records) <= 3, 'Bulk jobs require one to three records.')
        require(not bulk or family in type2.CAPACITIES, 'Unsupported bulk family.')
        require(records, 'Add at least one record.')
        if family != 'classic4k':
            type2.check_fit(family, records)
        require(type(overwrite) is bool and type(format) is bool, 'Invalid job options.')
        return cls(family, tuple(copy.deepcopy(records)), overwrite, format)


class Review:
    def __init__(self, job, snapshot, area, needs_format, binding, supplied):
        self.job, self.snapshot, self.area = job, copy.deepcopy(snapshot), area
        self.needs_format, self.binding = needs_format, binding
        self.supplied = supplied
        self.used = False

    @property
    def message(self):
        capacity = (self.area.capacity if self.area is not None else 716
                    if self.snapshot.card.classic else formatting.CC[self.snapshot.card.family][2]*8-2
                    - (2 if formatting.CC[self.snapshot.card.family][2]*8 > 256 else 0))
        records = '\n'.join(f'{i+1}. {record.display()[0]} · {record.display()[1][:200]}'
                            for i, record in enumerate(self.job.records))
        return (f'{self.snapshot.card.family} · UID {self.snapshot.card.uid.hex().upper()}\n'
                f'{len(self.job.records)} records · {len(ndef.encode(self.job.records))} / {capacity} encoded bytes\n'
                f'{records}\n'
                f'Overwrite: {"enabled" if self.job.overwrite else "off"}\n'
                + ('CANONICAL FORMAT: keys/access or CC and user data will change.\n' if self.needs_format
                   else 'Replace NDEF records; preserve UID and protected metadata.\n')
                + 'Write this target once and verify? Cancel is the default.')


def binding(client):
    backend = client.backend
    endpoint = getattr(backend, 'endpoint', None)
    if endpoint:
        return (endpoint.session, endpoint.generation, endpoint.token, backend.authentication)
    return (backend, getattr(backend, 'name', None), getattr(backend, 'connection', None),
            getattr(backend, 'owner', None), getattr(backend, 'authentication', b'\0'))


def area_for(snapshot, client):
    authenticated = getattr(client.backend, 'authentication', b'\0') != b'\0'
    return ntag.Type2NDEF(snapshot, authenticated) if snapshot.card.ntag else hf.ClassicNDEF(snapshot)


def empty_area(area):
    if area.records:
        return False
    rest = area.memory[len(area.prefix):]
    if not any(rest):
        return True
    if rest[:2] != b'\3\0':
        return False
    rest = rest[2:]
    if rest[:1] == b'\xfe':
        rest = rest[1:]
    return not any(rest)


def same_snapshot(current, expected):
    return (current.card == expected.card and current.errors == expected.errors
            and current.keys == expected.keys and current.blocks.keys() == expected.blocks.keys()
            and all((current.blocks[b][6:] == expected.blocks[b][6:]
                     if current.card.classic and b == hf.trailer_of(b)
                     else current.blocks[b] == expected.blocks[b]) for b in current.blocks))


async def prepare(client, job, supplied=hf.FACTORY):
    snapshot = await client.read(supplied)
    require(snapshot.card.family == job.family, f'Target is {snapshot.card.family}, selected {job.family}.')
    needs_format = False
    try:
        area = area_for(snapshot, client)
    except CardError:
        # A parse error alone never authorizes formatting: strict factory preflight must pass.
        require(job.format, 'Target has no supported NDEF layout; explicit canonical format permission is required.')
        if snapshot.card.classic:
            snapshot = await hf.ClassicClient(client.backend).read(hf.FACTORY, automatic=False)
        formatting.preflight(snapshot, job.overwrite)
        require(hasattr(client.backend, 'canonical_format'), 'Reader has no canonical formatter.')
        require(getattr(client.backend, 'authentication', b'\0') == b'\0',
                'Initialization cannot use supplied NTAG authentication.')
        if hasattr(client.backend, 'supports'):
            require(client.backend.supports('ntag_initialize' if snapshot.card.ntag else 'classic1k_format'),
                    'Firmware does not advertise canonical formatting support.')
        area, needs_format = None, True
    if area is not None:
        require(area.writable, 'NDEF layout is protected or unsupported.')
        area.replacing(job.records)
        require(job.overwrite or empty_area(area) or area.records == list(job.records),
                'Existing different NDEF records or data require explicit overwrite permission.')
    return Review(job, snapshot, area, needs_format, binding(client), supplied)


async def execute(client, review, supplied=hf.FACTORY):
    require(not review.used, 'Review already consumed.')
    review.used = True
    require(supplied == review.supplied, 'Credentials changed since review.')
    require(binding(client) == review.binding, 'Reader session changed since review.')
    current = await client.read(supplied)
    require(same_snapshot(current, review.snapshot),
            'Target bytes, permissions or credentials changed since review.')
    attempted = False
    try:
        if review.needs_format:
            attempted = True
            await client.canonical_format(review.snapshot, review.job.overwrite)
            current = await client.read(supplied)
            require(current.card == review.snapshot.card, 'Target changed after formatting.')
        area = area_for(current, client)
        if area.records == list(review.job.records):
            return current, 'Already correct; no record write.'
        attempted = True
        result = await client.write_records(list(review.job.records), area, supplied)
        return result, 'Formatted and verified.' if review.needs_format else 'Records verified.'
    except BaseException as exc:
        if attempted:
            raise CardError(f'Write unverified; set this target aside and read it before a new job. {exc}') from exc
        raise


async def run(factory, job, supplied, stop, progress, result, confirm, interval=.4):
    """Polling can only prepare: each target needs a fresh explicit confirmation."""
    removed = False
    while not stop.is_set():
        try:
            async with factory() as client:
                client.backend.cancelled = stop.is_set
                present = await client.backend.present()
                if not present:
                    removed = True
                    progress('Place a target; each target will need confirmation.')
                elif not removed:
                    progress('Remove the card before the next target.')
                else:
                    removed = False
                    write_phase = False
                    try:
                        review = await prepare(client, job, supplied)
                        if stop.is_set() or not await confirm(review) or stop.is_set():
                            review.used = True
                            progress('Cancelled target. Remove it before continuing.')
                        else:
                            write_phase = True
                            _, detail = await execute(client, review, supplied)
                            result(True, review.snapshot.card.uid.hex().upper() + ' · ' + detail)
                    except Exception as exc:
                        result(False, str(exc))
                        if write_phase:
                            # A failed mutation cannot be retried automatically, even after removal.
                            progress('Stopped after failed target; read it before starting a new job.')
                            return 'Bulk stopped: target write unverified. Read it before starting a new job; no automatic retry.'
                        progress('Target rejected before writing. Remove it before presenting another target.')
        except Exception as exc:
            removed = False
            progress(f'Reader error is not removal: {exc}')
        await asyncio.sleep(interval)
    return 'Bulk stopped. Check verified / unverified results before starting another job.'
