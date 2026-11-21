"""Generate deterministic CoreS3 bridge contract fixtures."""
import json
import struct
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SERVICE = uuid.uuid5(uuid.NAMESPACE_URL, 'https://midwestgadgets.org/quadband/cores3/bridge/v1')
UUIDS = {name: str(uuid.uuid5(SERVICE, name)) for name in ('rx', 'tx')}
UUIDS['service'] = str(SERVICE)
HEADER = '<2sBBQIBBHIH'


def frame(kind, session, request, channel, operation, generation, payload=b'', status=0):
    header = struct.pack(HEADER, b'QB', 1, kind, session, request, channel, operation, status, generation, len(payload))
    data = header+payload
    return data+struct.pack('<I', zlib.crc32(data))


def classic_geometry(sectors):
    result = []
    for sector in range(sectors):
        first = sector*4 if sector < 32 else 128+(sector-32)*16
        size = 4 if sector < 32 else 16
        result.append({'sector': sector, 'firstBlock': first, 'blockCount': size, 'trailer': first+size-1,
                       'accessGroups': [list(range(first+i, first+i+(1 if size == 4 else 5)))
                                        for i in ([0, 1, 2] if size == 4 else [0, 5, 10])]})
    return result


def profile(family, unit_size, unit_count, numbers):
    return {'format': 'quadband.card.data', 'version': 2, 'family': family,
            'geometry': {'unitSize': unit_size, 'unitCount': unit_count},
            'name': family+' fixture', 'createdAt': '2026-11-02T16:00:00Z',
            'blocks': [{'number': number, 'hex': ' '.join(['00']*unit_size)} for number in numbers]}

def mad_crc(data):
    crc = 0xC7
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1D if crc & 0x80 else 0)) & 255
    return crc


def mad2_fixture(geometry):
    sectors = [15]+list(range(17, 40))
    blocks = {n: bytes(16) for n in range(256)}
    for row in geometry:
        sector = row['sector']
        gpb = 0xC2 if sector == 0 else 0 if sector == 16 else 0x40
        access = bytes.fromhex('787788') if sector in (0, 16) else bytes.fromhex('7f0788')
        blocks[row['trailer']] = bytes(6)+access+bytes([gpb])+bytes(6)
    mad1, mad2 = bytearray(32), bytearray(48)
    mad1[1] = 1
    for sector in sectors:
        target, offset = (mad1, sector*2) if sector < 16 else (mad2, 2+(sector-17)*2)
        target[offset:offset+2] = b'\x03\xE1'
    mad1[0], mad2[0] = mad_crc(mad1[1:]), mad_crc(mad2[1:])
    blocks[1], blocks[2] = bytes(mad1[:16]), bytes(mad1[16:])
    for i, number in enumerate([64, 65, 66]):
        blocks[number] = bytes(mad2[i*16:i*16+16])
    ndef_blocks = [b for sector in sectors for b in range(geometry[sector]['firstBlock'], geometry[sector]['trailer'])]
    payload = b'\x02en'+b'X'*1000
    message = b'\xC1\x01'+len(payload).to_bytes(4, 'big')+b'T'+payload
    memory = (b'\x03\xFF'+len(message).to_bytes(2, 'big')+message+b'\xFE').ljust(len(ndef_blocks)*16, b'\0')
    for i, number in enumerate(ndef_blocks):
        blocks[number] = memory[i*16:i*16+16]
    return {'family': 'classic4k', 'uidHex': '01020304', 'atqaHex': '0200', 'sak': 0x18,
            'ndefSectors': sectors, 'ndefBlocks': ndef_blocks, 'messageHex': message.hex(),
            'mad1Hex': mad1.hex(), 'mad2Hex': mad2.hex(), 'sector0GPB': 0xC2, 'sector16GPB': 0,
            'blocks': [{'number': n, 'hex': value.hex()} for n, value in blocks.items()]}


def main():
    manifest = {'protocol': 1, 'name': 'Quadband CoreS3', 'uuids': UUIDS,
                'headerFormat': HEADER, 'headerSize': struct.calcsize(HEADER), 'maxPayload': 2048,
                'maxFrame': 2078, 'maxBuffer': 8312,
                'kinds': {'request': 0, 'response': 1, 'event': 2},
                'channels': {'control': 0, 'hf': 1, 'uhf': 2, 'chameleon': 3},
                'operations': {'hello': 1, 'capabilities': 2, 'ping': 3, 'cancel': 4,
                               'identify': 16, 'classicRead': 17, 'classicWrite': 18,
                               'ntagRead': 19, 'ntagWrite': 20, 'resetAuth': 21,
                               'classicFormat1k': 22, 'ntagInitialize': 23,
                               'transact': 32, 'peripheralsChanged': 128},
                'statuses': dict(zip(['ok', 'badFrame', 'badVersion', 'notAuthorized', 'badSession',
                                     'unavailable', 'staleGeneration', 'busy', 'timeout', 'cancelled',
                                     'noCard', 'authFailed', 'unsupported', 'invalidArgument', 'ioError',
                                     'writeUnverified', 'permissionDenied', 'staleData'], range(18)))}
    caps = {'protocol': 1, 'deviceId': 'fixture-board', 'name': 'Quadband CoreS3', 'firmware': '0.1.0',
            'clientNonce': '1122334455667788',
            'peripherals': [{'channel': 1, 'generation': 3, 'present': True, 'kind': 'rfid2',
                             'capabilities': ['identify', 'classic1k', 'classic4k', 'ntag21x', 'ntag_auth']},
                            {'channel': 2, 'generation': 7, 'present': True, 'kind': 'jrd4035',
                             'capabilities': ['m100']},
                            {'channel': 3, 'generation': 1, 'present': False, 'kind': 'chameleon',
                             'capabilities': []}]}
    payloads = [('helloRequest', 0, 0, 1, 0, 1, 0, bytes.fromhex('1122334455667788'), 0),
                ('helloResponse', 1, 0x0102030405060708, 1, 0, 1, 0,
                 json.dumps(caps, separators=(',', ':')).encode(), 0),
                ('classicRead', 0, 0x0102030405060708, 2, 1, 17, 3,
                 bytes.fromhex('0401020304800060FFFFFFFFFFFF'), 0),
                ('classicReadResponse', 1, 0x0102030405060708, 2, 1, 17, 3, bytes(range(16)), 0),
                ('ntagRead', 0, 0x0102030405060708, 3, 1, 19, 3,
                 bytes.fromhex('070401020304050604000400'), 0),
                ('uhfIdentify', 0, 0x0102030405060708, 4, 2, 32, 7,
                 bytes.fromhex('BB0003000100047E'), 0),
                ('noCard', 1, 0x0102030405060708, 5, 1, 16, 3, b'Place one card on the reader.', 10),
                ('embeddedMarkers', 1, 0x0102030405060708, 6, 0, 3, 0, b'QB\x00\x7e\xbb\x11\xef', 0)]
    frames = []
    for name, kind, session, request, channel, operation, generation, data, status in payloads:
        frames.append({'name': name, 'kind': kind, 'session': str(session), 'requestId': request,
                       'channel': channel, 'operation': operation, 'generation': generation,
                       'status': status, 'payloadHex': data.hex(),
                       'frameHex': frame(kind, session, request, channel, operation, generation, data, status).hex()})
    geometries = {'classic1k': classic_geometry(16), 'classic4k': classic_geometry(40)}
    classic4k_trailers = {row['trailer'] for row in geometries['classic4k']}
    profiles = [profile('classic4k', 16, 256, [n for n in range(1, 256) if n not in classic4k_trailers])]
    for family, total, end, lock in [('ntag213', 45, 39, 40), ('ntag215', 135, 129, 130), ('ntag216', 231, 225, 226)]:
        geometries[family] = {'unitSize': 4, 'unitCount': total, 'firstUserPage': 4, 'lastUserPage': end,
                              'dynamicLockPage': lock, 'firstConfigPage': lock+1}
        profiles.append(profile(family, 4, total, range(4, end+1)))
    legacy = {'format': 'quadband.classic1k.data', 'version': 1, 'name': 'Classic 1K fixture',
              'createdAt': '2026-11-02T16:00:00Z',
              'blocks': [{'number': n, 'hex': ' '.join(['00']*16)} for n in range(1, 64) if n % 4 != 3]}
    lock_cases = []
    for family, end, stride, last_bit, block_mask in [('ntag213', 39, 2, 11, 0x3F),
                                                      ('ntag215', 129, 16, 7, 0x0F),
                                                      ('ntag216', 225, 16, 13, 0x7F)]:
        cases = [('unlocked', b'\0\0', bytes([0,0,0,0xBD]), []),
                 ('blockLockOnly', b'\7\0', bytes([0,0,block_mask,0xBD]), []),
                 ('staticPage4', b'\x10\0', bytes([0,0,0,0xBD]), [4]),
                 ('staticPage15', b'\0\x80', bytes([0,0,0,0xBD]), [15])]
        for name, bit in [('dynamicFirst', 0), ('dynamicSecond', 1), ('dynamicLast', last_bit)]:
            start = 16+bit*stride
            dynamic = (1 << bit).to_bytes(2, 'little')+bytes([0,0xBD])
            cases.append((name, b'\0\0', dynamic, list(range(start, min(start+stride, end+1)))))
        for name, static, dynamic, pages in cases:
            lock_cases.append({'name': family+'-'+name, 'family': family, 'staticLockHex': static.hex(),
                               'dynamicLockPageHex': dynamic.hex(), 'lockedUserPages': pages})
    tlv_cases = [{'family': 'ntag213', 'lockControlHex': 'a00c34', 'lockByteAddress': 160},
                 {'family': 'ntag215', 'lockControlHex': '880866', 'lockByteAddress': 520},
                 {'family': 'ntag216', 'lockControlHex': 'e80e66', 'lockByteAddress': 904}]
    fixtures = {'protocol': 1, 'capabilities': caps, 'frames': frames, 'geometries': geometries,
                'profiles': profiles, 'legacyProfiles': [legacy], 'ntagLockCases': lock_cases,
                'ntagLockControlCases': tlv_cases, 'classic4kNdef': mad2_fixture(geometries['classic4k'])}
    (ROOT/'fixtures').mkdir(exist_ok=True)
    (ROOT/'cores3-bridge-v1.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (ROOT/'fixtures/cores3-v1.json').write_text(json.dumps(fixtures, indent=2)+'\n')
    print(f'Generated {len(frames)} wire frames, {len(geometries)} geometries and {len(profiles)} profiles.')


if __name__ == '__main__':
    main()
