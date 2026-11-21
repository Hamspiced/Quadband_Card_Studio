"""Fixed-layout, explicitly reviewed formatting. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from .common import CardError, require
from .hf import FACTORY, NTAG_LAYOUTS, mad_crc

VERSIONS = {name: bytes.fromhex('000404020100') + bytes([size, 3])
            for name, size in zip(NTAG_LAYOUTS, (15, 17, 19))}
CC = dict(zip(NTAG_LAYOUTS, map(bytes.fromhex, ('e1101200', 'e1103e00', 'e1106d00'))))
DATA_TRAILER = bytes.fromhex('d3f7d3f7d3f77f078840ffffffffffff')
DIRECTORY_TRAILER = bytes.fromhex('a0a1a2a3a4a5787788c1ffffffffffff')


def directory():
    mad = bytearray(b'\0\x01' + b'\x03\xe1' * 15)
    mad[0] = mad_crc(mad[1:])
    return bytes(mad)


def canonical_unit(card, unit, data, credential):
    """Validate even the private physical sink against the fixed output."""
    if card.ntag:
        require(credential is None and card.version == VERSIONS[card.family], 'Invalid initialization identity.')
        count = CC[card.family][2] * 2
        expected = (CC[card.family] if unit == 3 else b'\x03\0\xfe\0' if unit == 4 else bytes(4))
        require(3 <= unit < 4+count and data == expected, 'Only canonical NTAG initialization bytes are allowed.')
    else:
        require(card.family == 'classic1k' and credential == FACTORY and 1 <= unit < 64,
                'Only canonical factory Classic1K formatting is allowed.')
        expected = (directory()[(unit-1)*16:unit*16] if unit in (1, 2)
                    else DIRECTORY_TRAILER if unit == 3 else DATA_TRAILER if unit % 4 == 3
                    else b'\x03\0\xfe'.ljust(16, b'\0') if unit == 4 else bytes(16))
        require(data == expected, 'Only fixed canonical Classic1K bytes are allowed.')


def snapshot_bytes(snapshot):
    card = snapshot.card
    count = 64 if card.family == 'classic1k' else card.unit_count - 2 if card.ntag else 0
    require(count > 0 and not snapshot.errors and set(snapshot.blocks) == set(range(count))
            and all(len(snapshot.blocks.get(i, b'')) == card.unit_size for i in range(count)),
            'Formatting needs a complete readable snapshot.')
    return b''.join(snapshot.blocks[i] for i in range(count))


def preflight(snapshot, overwrite=False):
    require(type(overwrite) is bool, 'Invalid overwrite permission.')
    snapshot_bytes(snapshot)
    card, blocks = snapshot.card, snapshot.blocks
    if card.ntag:
        require(card.version == VERSIONS[card.family], 'Exact NTAG GET_VERSION identity is required.')
        dynamic = card.unit_count - 5
        require(blocks[3] == bytes(4), 'Only an all-zero CC may be initialized; existing CC is never repaired.')
        require(blocks[2][2:] == bytes(2) and blocks[dynamic][:3] == bytes(3)
                and blocks[dynamic+1][3] == 255, 'NTAG locks or password protection prevent initialization.')
        require(overwrite or not any(b''.join(blocks[i] for i in range(4, card.unit_count-5))),
                'Tag already holds data; explicit overwrite permission is required.')
    else:
        require(card.family == 'classic1k' and card.unit_count == 64, 'Only factory Classic1K can be formatted.')
        require(blocks[3][9] not in (0xc1, 0xc2), 'Existing or partial MAD formatting cannot be repaired.')
        require(all(snapshot.keys.get(s) == FACTORY for s in range(16)),
                'Every sector must be read using the factory key A.')
        require(all(blocks[b][6:9] == bytes.fromhex('ff0780') and blocks[b][10:] == b'\xff'*6
                    for b in range(3, 64, 4)),
                'Every sector must have factory access conditions and key B.')
        require(overwrite or not any(b''.join(blocks[b] for b in range(1, 64) if b % 4 != 3)),
                'Tag already holds data; explicit overwrite permission is required.')


async def canonical(backend, snapshot, overwrite=False):
    """Execute only fixed writes; private backend writers are never general public APIs."""
    preflight(snapshot, overwrite)
    require(getattr(backend, 'authentication', b'\0') == b'\0',
            'Canonical formatting cannot use supplied NTAG authentication.')
    require(await backend.identify() == snapshot.card, 'Card changed since review.')
    card = snapshot.card
    if card.ntag:
        from .ntag import Client
        current = await Client(backend).read()
    else:
        from .hf import ClassicClient
        current = await ClassicClient(backend).read(FACTORY, automatic=False)
    require(current.card == card, 'Card changed during formatting preflight.')
    preflight(current, overwrite)
    require(all((current.blocks[b][6:] == snapshot.blocks[b][6:] if card.classic and b % 4 == 3
                 else current.blocks[b] == snapshot.blocks[b]) for b in snapshot.blocks),
            'Card bytes changed since review.')
    attempted = False

    async def put(unit, data, credential=None, new_credential=None):
        nonlocal attempted
        await asyncio.sleep(0)
        require(not getattr(backend, 'cancelled', lambda: False)(), 'Formatting cancelled.')
        require(await backend.identify() == card, 'Card changed during formatting.')
        old = (await backend.read_pages(unit) if card.ntag
               else await backend.read_block(unit, credential))
        require((old == current.blocks[unit] if card.ntag or unit % 4 != 3
                 else old[6:] == current.blocks[unit][6:]), 'Card bytes changed during formatting.')
        if card.ntag:
            dynamic = card.unit_count-5
            for metadata in (2, 3, dynamic, dynamic+1, dynamic+2):
                require(await backend.read_pages(metadata) == current.blocks[metadata],
                        'NTAG permissions changed during initialization.')
        else:
            trailer = (unit//4)*4+3
            permissions = await backend.read_block(trailer, credential)
            require(permissions[6:] == current.blocks[trailer][6:],
                    'Classic permissions changed during formatting.')
        require(await backend.identify() == card, 'Card changed before formatting mutation.')
        require(not getattr(backend, 'cancelled', lambda: False)(), 'Formatting cancelled.')
        attempted = True
        await backend._format_unit(unit, data, credential)
        if not card.ntag:
            await backend.reset_authentication()
        result = (await backend.read_pages(unit) if card.ntag
                  else await backend.read_block(unit, new_credential or credential))
        require((result == data if card.ntag or unit % 4 != 3 else result[6:10] == data[6:10]),
                'Canonical format verification failed.')
        require(await backend.identify() == card, 'Card changed during formatting.')
        current.blocks[unit] = result

    try:
        if card.ntag:
            count = CC[card.family][2] * 2
            image = b'\x03\x00\xfe'.ljust(count * 4, b'\0')
            for i in range(count):
                if current.blocks[4+i] != image[i*4:i*4+4]:
                    await put(4+i, image[i*4:i*4+4])
            await put(3, CC[card.family])
        else:
            image = b'\x03\x00\xfe'.ljust(720, b'\0')
            for sector in range(1, 16):
                for offset in range(3):
                    block = sector*4+offset
                    data = image[((sector-1)*3+offset)*16:((sector-1)*3+offset+1)*16]
                    if current.blocks[block] != data:
                        await put(block, data, FACTORY)
                await put(sector*4+3, DATA_TRAILER, FACTORY, (DATA_TRAILER[:6], 0x60))
            mad = directory()
            for block in (1, 2):
                await put(block, bytes(mad[(block-1)*16:block*16]), FACTORY)
            await put(3, DIRECTORY_TRAILER, FACTORY, (DIRECTORY_TRAILER[:6], 0x60))
    except BaseException as exc:
        if attempted:
            raise CardError('Formatting write unverified; set this tag aside. No automatic retry.') from exc
        raise
