"""Password-free T5577 programming. SPDX-License-Identifier: GPL-3.0-only

Independent encoders for the Chameleon 3016 block-write protocol.
Only page 0 blocks 0–6, no password authentication, no lock bits, no block 7.
See docs/T5577.txt for protocol references and limits.
"""
from functools import reduce
from operator import xor
from .common import require

LEGACY_PASSWORD_WRITERS = frozenset([3001, 3003, 3005, 3006, 3011, 3015, 3018, 3020])
FORMATS = frozenset(['EM410x', 'EM410x Electra', 'Viking', 'ioProx', 'PAC / Stanley', 'Jablotron'])


def guard(command, data):
    require(command not in LEGACY_PASSWORD_WRITERS,
            'This command sets a T5577 password and is disabled. Use password-free block writing.')
    if command == 3016:
        require(len(data) == 11 and data[0] < 7 and data[5:] == bytes(6),
                'Only open page-0 writes to blocks 0–6 are allowed. Password writes are disabled.')
        if data[0] == 0:
            require(not int.from_bytes(data[1:5], 'big') & 0x10, 'T5577 password protection must remain disabled.')


def payload(block, word):
    data = bytes([block])+word.to_bytes(4, 'big')+bytes(6)
    guard(3016, data)
    return data


def blocks(card):
    require(card.format in FORMATS, 'Password-free writing of this LF format is not implemented; reading and saving remain available.')
    data = card.data
    if card.format in ('EM410x', 'EM410x Electra'):
        bits, columns = '1'*9, 0
        for byte in data[:5]:
            for nibble in (byte >> 4, byte & 15):
                bits += f'{nibble:04b}'+str(nibble.bit_count() % 2)
                columns ^= nibble
        raw = int(bits+f'{columns:04b}'+'0', 2).to_bytes(8, 'big')
        if card.format == 'EM410x Electra':
            raw += data[5:]
        config = 0x00148000 | ((len(raw)//4) << 5)
    elif card.format == 'Viking':
        raw = b'\xf2\0\0'+data+bytes([reduce(xor, data, 0x5A)])
        config = 0x00088040
    elif card.format == 'ioProx':
        raw, config = data[4:12], 0x00147040
    elif card.format == 'Jablotron':
        raw, config = b'\xff\xff'+data+bytes([(sum(data) ^ 0x3A) & 255]), 0x00158040
    else:
        body = b'\x02' + b'20' + data + bytes([reduce(xor, data, 0)])
        bits = '1'*8
        for byte in body:
            seven = ''.join(str((byte >> i) & 1) for i in range(7))
            bits += '0'+seven+str(1-(seven.count('1') % 2))+'1'
        raw, config = int(bits, 2).to_bytes(16, 'big'), 0x00080080
    require(not config & 0x10 and len(raw) <= 24, 'Invalid password-free configuration.')
    # Data first, configuration last. No password storage block is ever touched.
    return [(i//4+1, int.from_bytes(raw[i:i+4], 'big')) for i in range(0, len(raw), 4)] + [(0, config)]
