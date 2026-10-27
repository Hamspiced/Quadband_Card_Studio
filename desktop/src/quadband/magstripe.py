"""MSR wire protocol and keyboard parsing. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import re
from .common import CardError, require

SHORT = ['MIDWEST TEST 12345', '1234567890=12345', '9876543210=54321']
SAMPLE = ['B4111111111111111^GADGETS/MIDWEST^2912101000000000000000',
          '4111111111111111=29121010000000000000',
          '011234567890=4111111111111111=840=0000=0000=0000000000000000']
MAPPINGS = {'Automatic · track markers': None, 'Tracks 1, 2, 3': [0, 1, 2],
            'Tracks 1, 2': [0, 1], 'Tracks 2, 3': [1, 2],
            'Track 1': [0], 'Track 2': [1], 'Track 3': [2]}


def normalize(text, track):
    text = text.strip('\r\n')
    if text.startswith('%' if track == 0 else ';') or (track == 2 and text.startswith('+')):
        text = text[1:]
    return text[:-1] if text.endswith('?') else text


def validate(tracks, densities=(210, 75, 210)):
    require(len(tracks) == len(densities) == 3, 'Three tracks and densities are required.')
    tracks = [normalize(t, i) for i, t in enumerate(tracks)]
    require(any(tracks), 'Enter at least one track.')
    for i, (text, density) in enumerate(zip(tracks, densities)):
        require(density in (75, 210), 'Choose 75 or 210 BPI.')
        valid = all(32 <= ord(c) <= 95 and c not in '?%' for c in text) if i == 0 else all(
            48 <= ord(c) <= 62 and c != ';' for c in text)
        require(valid, f'Track {i+1}: use uppercase ASCII for Track 1, or digits and : < = > for Tracks 2/3.')
        limit = (25 if i == 0 else 37) if density == 75 else (76 if i == 0 else 104)
        require(len(text) <= limit, f'Track {i+1} exceeds {limit} characters at {density} BPI.')
    return tracks


def parse_swipe(raw, mapping=None):
    raw = re.sub(r'\r\n|\r', '\n', raw)
    if raw.endswith('\n'):
        raw = raw[:-1]
    raw = raw.removeprefix('\x02').removesuffix('\x03')
    result = ['', '', '']
    if mapping is not None:
        fields = re.split('[\n\t]', raw)
        require(len(fields) == len(mapping), f'This mapping expects {len(mapping)} fields separated by Enter or Tab.')
        for i, text in zip(mapping, fields):
            result[i] = normalize(text, i)
    else:
        seen, cursor = set(), 0
        while cursor < len(raw):
            if raw[cursor] in '\n\t':
                cursor += 1
                continue
            marker = raw[cursor]
            require(marker in '%;+', 'Swipe captured without markers. Choose a field mapping to interpret the raw swipe.')
            track = 0 if marker == '%' else (2 if marker == '+' or 1 in seen else 1)
            require(track not in seen, 'Duplicate tracks. Clear and swipe one card.')
            end = raw.find('?', cursor + 1)
            require(end >= 0, 'Incomplete swipe: missing end sentinel (?).')
            result[track] = raw[cursor+1:end]
            seen.add(track)
            cursor = end + 1
    require(any(result), 'No track data received.')
    return result


def hid_packets(message):
    return [bytes([len(chunk) | (0x80 if i == 0 else 0) | (0x40 if i+63 >= len(message) else 0)])
            + chunk + bytes(63-len(chunk)) for i in range(0, len(message), 63)
            for chunk in [message[i:i+63]]]


class HIDDecoder:
    def __init__(self):
        self.data = None

    def feed(self, packet):
        require(bool(packet), 'Empty USB report.')
        header, size = packet[0], packet[0] & 63
        require(size > 0 and len(packet) >= size+1, 'Truncated USB report.')
        if header & 0x80:
            require(self.data is None, 'USB message interrupted.')
            self.data = b''
        require(self.data is not None, 'USB report has no start marker.')
        self.data += packet[1:1+size]
        require(len(self.data) <= 4096, 'USB report is too large.')
        if header & 0x40:
            result, self.data = self.data, None
            return result


class StreamDecoder:
    def __init__(self, command):
        self.command, self.data = command[1], b''

    def feed(self, chunk):
        self.data += chunk
        require(len(self.data) <= 4096, 'MSR response is too large.')
        if len(self.data) < 2:
            return None
        require(self.data[0] == 0x1B, 'Unexpected MSR response; reconnect.')
        size = 5 if self.command == 0x6F and self.data[1] == 0x30 else 2
        if self.command == 0x72 and self.data[1] == 0x73:
            end = self.data.find(b'?\x1c\x1b', 2)
            if end < 0:
                return None
            size = end + 4
        if len(self.data) < size:
            return None
        require(len(self.data) == size, 'Unexpected trailing MSR data; reconnect.')
        return self.data


def status(reply):
    require(len(reply) >= 2 and reply[0] == 27, 'Malformed writer acknowledgement.')
    errors = {0x31: 'Read/write error; check swipe, orientation and coercivity.',
              0x32: 'Writer rejected the data format.', 0x34: 'Unsupported command.',
              0x39: 'Invalid swipe; swipe steadily through the slot.', 0x41: 'Writer rejected a setting.'}
    require(reply[1] == 0x30, errors.get(reply[1], f'Writer status {reply[1]:02X}.'))


def read_tracks(data):
    if len(data) == 2:
        status(data)
        raise CardError('The writer returned no track data.')
    require(data.startswith(b'\x1bs') and data[-4:-1] == b'?\x1c\x1b', 'Malformed card response.')
    status(data[-2:])
    tracks, seen, cursor, end = ['', '', ''], set(), 2, len(data)-4
    while cursor < end:
        require(cursor+1 < end and data[cursor] == 27 and data[cursor+1] in (1, 2, 3), 'Invalid track marker.')
        track = data[cursor+1]-1
        require(track not in seen, 'Duplicate track in response.')
        seen.add(track)
        cursor += 2
        next_marker = data.find(b'\x1b', cursor, end)
        next_marker = end if next_marker < 0 else next_marker
        try:
            tracks[track] = normalize(data[cursor:next_marker].decode('ascii'), track)
        except UnicodeError as exc:
            raise CardError('Non-ASCII track data.') from exc
        cursor = next_marker
    require(any(tracks), 'No readable tracks.')
    return tracks


class MSRClient:
    def __init__(self, channel):
        self.channel = channel

    async def exchange(self, message, timeout=3):
        decoder = HIDDecoder() if self.channel.framed_hid else StreamDecoder(message)
        try:
            await self.channel.send(message)
            async with asyncio.timeout(timeout):
                while True:
                    result = decoder.feed(await self.channel.receive())
                    if result is not None:
                        return result
        except BaseException:
            await self.channel.close()
            raise

    async def connect(self):
        await self.channel.send(b'\x1ba')
        await asyncio.sleep(.15)
        require(await self.exchange(b'\x1be') == b'\x1by', 'This device did not identify as an MSR-compatible writer.')
        return 'MSR writer connected'

    async def read(self):
        return read_tracks(await self.exchange(b'\x1br', 45))

    async def write_verify(self, tracks, densities, high_coercivity, progress):
        tracks = validate(tracks, densities)
        commands = [bytes([27, 0x78 if high_coercivity else 0x79]), b'\x1bo\x07\x05\x05',
                    bytes([27, 0x62, 0xA1 if densities[0] == 210 else 0xA0]),
                    bytes([27, 0x62, 0xD2 if densities[1] == 210 else 0x4B]),
                    bytes([27, 0x62, 0xC1 if densities[2] == 210 else 0xC0]),
                    bytes([27, 0x7A, 22 if 75 in (densities[0], densities[2]) else 61,
                           22 if densities[1] == 75 else 61])]
        for command in commands:
            reply = await self.exchange(command)
            status(reply)
            if command[1] == 0x6F:
                require(reply == b'\x1b0\x07\x05\x05', 'Writer did not confirm 7/5/5 bits per character.')
        progress('Swipe the target card to write. You have 45 seconds.')
        message = b'\x1bw\x1bs' + b''.join(bytes([27, i+1])+s.encode('ascii') for i, s in enumerate(tracks)) + b'?\x1c'
        try:
            status(await self.exchange(message, 45))
            progress('Write acknowledged. Swipe the SAME card again to verify.')
            actual = await self.read()
            require(all(not desired or actual[i] == desired for i, desired in enumerate(tracks)),
                    'The written tracks did not match on read-back.')
        except Exception as exc:
            raise CardError(f'Write not verified; the card may have changed. {exc}') from exc
        return 'Requested tracks verified. Empty tracks were not certified.'
