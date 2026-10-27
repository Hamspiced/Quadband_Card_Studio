"""Independent ACS reader controls. SPDX-License-Identifier: GPL-3.0-only

Protocol reference: ACS ACR122U API 2.04 / ACR1552U Reference 1.09.
RF preset values credited to Tototo31 and Ashsh, Dangerous Things forum.
No third-party script code is incorporated.
"""
import asyncio
from dataclasses import dataclass
import re
from .common import CardError, require

REGISTERS = {'polling': (0x23, 1), 'types': (0x20, 2), 'pps': (0x24, 1), 'ui': (0x21, 1),
             'hid': (0x90, 2), 'characters': (0x91, 3), 'keyboard': (0x92, 1), 'suspend': (0xE5, 1),
             'host': (0x93, 1), 'power': (0x50, 1), 'rf': (0x25, 1), 'usbSerial': (0xF0, 1),
             'operating': (0x50, 1), 'buzzer': (0x52, 1)}
WRITE_ONLY = {'rf', 'usbSerial', 'buzzer'}


@dataclass
class Setting:
    id: str
    title: str
    register: str
    mask: int
    choices: dict
    index: int = 0
    feature: str = ''

    def value(self, snapshot):
        data = snapshot['values'].get(self.register)
        if data is None:
            return None
        if self.id == 'pulse':
            return [250, 500, 1000, 2500][(data[0] >> 4) & 3] if data[0] & 2 else 0
        return data[self.index] & self.mask


def toggle(id, title, register, mask, index=0, feature=''):
    return Setting(id, title, register, mask, {0: 'Off', mask: 'On'}, index, feature)


def fields(family):
    if family == 'ACR122U':
        return [toggle('buzzer', 'Card-detection buzzer', 'buzzer', 255),
                toggle('polling', 'Automatic card detection', 'operating', 128),
                Setting('interval', 'Polling interval', 'operating', 32, {32: '250 ms', 0: '500 ms'}),
                toggle('ats', 'Automatic ATS', 'operating', 64),
                Setting('rf', 'RF field now', 'rf', 255, {0: 'Off', 1: 'On'}),
                *[toggle(id, title, 'operating', mask) for id, title, mask in
                  [('typeA', 'Type A / MIFARE', 1), ('typeB', 'Type B', 2), ('topaz', 'Topaz', 4),
                   ('felica212', 'FeliCa 212', 8), ('felica424', 'FeliCa 424', 16)]]]
    result = [toggle('presenceBeep', 'Card-present buzzer', 'ui', 8), toggle('removalBeep', 'Card-removal buzzer', 'ui', 16),
              toggle('powerBeep', 'Power-up buzzer', 'ui', 32, feature='newUI'),
              Setting('power', 'RF power (restarts reader)', 'power', 255, {0: 'Hardware default', 1: '20%', 2: '40%', 3: '60%', 4: '80%', 5: '100%'}, feature='power'),
              Setting('pulse', 'RF-off interval (replug to activate)', 'polling', 0x32, {0: 'Continuous', 250: '250 ms', 500: '500 ms', 1000: '1,000 ms', 2500: '2,500 ms'}),
              toggle('polling', 'Automatic card detection', 'polling', 1),
              Setting('pps', 'Maximum connection speed', 'pps', 255, {0: '106 kbps', 1: '212 kbps', 2: '424 kbps', 3: '848 kbps'}),
              Setting('rf', 'RF field now', 'rf', 255, {0: 'Off', 1: 'On with polling', 2: 'On without polling'}),
              toggle('mifareATR', 'MIFARE type identification', 'polling', 8), toggle('part4ATR', 'Part 4 identification', 'polling', 128),
              toggle('accessLED', 'Activity LED', 'ui', 1), toggle('pollingLED', 'Polling LED', 'ui', 2, feature='newUI'),
              toggle('activationLED', 'Card-activation LED', 'ui', 4, feature='newUI'),
              toggle('suspend', 'USB selective suspend', 'suspend', 1, feature='suspend'),
              Setting('host', 'USB mode', 'host', 255, {1: 'Card reader (CCID)', 2: 'Reader + keyboard', 0: 'Keyboard only (removes PC/SC)'}),
              toggle('uppercase', 'Uppercase UID', 'hid', 16),
              Setting('uidLength', 'UID lengths', 'hid', 32, {0: '4 bytes', 32: '4, 7, 8 or 10 bytes'}),
              Setting('hidMode', 'UID format', 'hid', 15, dict(enumerate(['Hexadecimal', 'Decimal per byte', 'Decimal', '6H-6H', '8H-8H', '10H-10H', '14H-14H', '20H-20H', '6H-8D', '6H-10D', '8H-10D', '10H-14D', '2H4H-8D', '14H-17D']))),
              Setting('hidOrder', 'UID byte order', 'hid', 255, {0: 'Normal', 1: 'Reversed', 2: 'Reverse ISO 14443 / FeliCa', 4: 'Reverse ISO 15693'}, index=1),
              Setting('keyboard', 'Keyboard language', 'keyboard', 255, {0: 'English', 1: 'French', 3: 'Lithuanian'}),
              toggle('usbSerial', 'Include serial in USB descriptor', 'usbSerial', 1)]
    for id, title, index in [('uidStart', 'Character before UID', 2), ('uidBetween', 'Character between UID bytes', 0), ('uidEnd', 'Character after UID', 1)]:
        result.append(Setting(id, title, 'characters', 255, {255: 'None', 0x28: 'Enter', 0x2C: 'Space', 0x2D: 'Hyphen', 0x33: 'Semicolon', 0x36: 'Comma', 0x37: 'Period'}, index))
    for id, title, index, bit in [('typeA', 'ISO 14443 Type A', 0, 0), ('typeB', 'ISO 14443 Type B', 0, 1),
                                ('felica', 'FeliCa', 0, 2), ('topaz', 'Topaz', 0, 4), ('innovatron', 'Innovatron', 0, 5),
                                ('sri', 'SRI / SRIX', 0, 6), ('picopassB', 'PicoPass Type B', 1, 0), ('picopass15693', 'PicoPass 15693', 1, 1),
                                ('iso15693', 'ISO 15693', 1, 2), ('cts', 'CTS', 1, 3)]:
        result.append(toggle(id, title, 'types', 1 << bit, index))
    return result


def supports(snapshot, feature):
    if not feature:
        return True
    version = re.search(r'(\d+)\.(\d+)\.(\d+)', snapshot['firmware'])
    if not version:
        return False
    major, minor, patch = map(int, version.groups())
    number = minor*100+patch
    if feature == 'power':
        return major == 1 and number >= 303
    if feature == 'newUI':
        return major == 1 and number >= 600
    return major == 1 and number >= 500 or major == 5 and number >= 100


def preset(snapshot, name):
    if name in ('mute', 'unmute'):
        on = name == 'unmute'
        if snapshot['family'] == 'ACR122U':
            return {'buzzer': 255 if on else 0}
        return {f.id: f.mask if on else 0 for f in fields('ACR1552U')
                if f.id in ('presenceBeep', 'removalBeep', 'powerBeep') and supports(snapshot, f.feature)}
    require(snapshot['family'] == 'ACR1552U', 'RF power presets require an ACR1552U.')
    return {'tototo31': {'power': 4, 'pulse': 250}, 'ashsh': {'power': 5, 'pulse': 0}, 'rfDefaults': {'power': 0, 'pulse': 250}}[name]


def plan(snapshot, edits):
    desired, descriptions = {}, {}
    catalog = {f.id: f for f in fields(snapshot['family'])}
    for id, value in edits.items():
        require(id in catalog and value in catalog[id].choices, 'Unknown reader setting.')
        f = catalog[id]
        require(supports(snapshot, f.feature), f'{f.title} requires newer compatible firmware.')
        require(f.register in WRITE_ONLY or f.register in snapshot['values'], f'Read {f.title} before changing it.')
        require(id != 'hidOrder' or value not in (2, 4) or supports(snapshot, 'suspend'), 'Selective UID reversal requires newer firmware.')
        if f.value(snapshot) == value:
            continue
        data = desired.setdefault(f.register, bytearray(snapshot['values'].get(f.register, bytes(REGISTERS[f.register][1]))[:REGISTERS[f.register][1]]))
        if id == 'pulse':
            data[0] = data[0] & ~2 if value == 0 else (data[0] & ~0x32) | 2 | ([250, 500, 1000, 2500].index(value) << 4)
        else:
            data[f.index] = (data[f.index] & ~f.mask) | value
        descriptions.setdefault(f.register, []).append(f'{f.title}: {f.choices[value]}')
    if snapshot['family'] == 'ACR1552U':
        if 'types' in desired:
            desired['types'][0] &= 0x77
            desired['types'][1] &= 15
            require(any(desired['types']), 'Keep at least one card technology enabled.')
        if 'host' in desired or 'suspend' in desired:
            host = desired.get('host', snapshot['values'].get('host'))
            suspend = desired.get('suspend', snapshot['values'].get('suspend'))
            require(not (suspend == b'\1' and host != b'\1'), 'Selective suspend requires CCID-only mode.')
            require(not (host != b'\1' and supports(snapshot, 'suspend') and suspend is None), 'Read suspend state before enabling keyboard mode.')
    elif 'operating' in desired:
        require(not desired['operating'][0] & 128 or desired['operating'][0] & 31, 'Keep a card technology enabled while polling.')
    order = ['polling', 'types', 'pps', 'ui', 'hid', 'characters', 'keyboard', 'operating', 'buzzer', 'usbSerial']
    order += ['suspend', 'host'] if desired.get('suspend') == b'\0' else ['host', 'suspend']
    order += ['power', 'rf']
    if desired.get('host') == b'\0':
        order.remove('host')
        order.append('host')
    return [(reg, bytes(desired[reg]), '; '.join(descriptions[reg])) for reg in order if reg in desired]


def escape_payload(reply):
    require(len(reply) >= 5 and reply[:4] == b'\xe1\0\0\0' and len(reply) == 5+reply[4], 'Invalid ACS escape reply.')
    return reply[5:]


def operating_byte(reply):
    if len(reply) == 1:
        return reply
    require(len(reply) == 2 and reply[0] == 0x90, 'Invalid ACR122U polling reply.')
    return reply[1:]


class ACS:
    def __init__(self, control, reader):
        self.control, self.reader = control, reader
        require('SAM' not in reader.upper(), 'Select the contactless reader interface.')
        self.family = 'ACR1552U' if re.search(r'\bACR1552U?\b', reader.upper()) else 'ACR122U' if re.search(r'\bACR122U?\b', reader.upper()) else None
        require(self.family is not None, 'Reader controls support ACR122U and ACR1552U.')

    async def firmware(self):
        raw = await self.control(bytes.fromhex('FF00480000') if self.family == 'ACR122U' else bytes.fromhex('E000001800'))
        data = raw if self.family == 'ACR122U' else escape_payload(raw)
        require(0 < len(data) <= 80 and all(32 <= c < 127 for c in data), 'Invalid reader firmware identity.')
        text = data.decode('ascii')
        require(text.startswith('ACR122U' if self.family == 'ACR122U' else 'ACR1552'), 'Unexpected reader identity.')
        return text

    async def read_register(self, reg):
        raw = await self.control(bytes.fromhex('FF00500000') if self.family == 'ACR122U'
                                 else bytes([0xE0, 0, 1 if reg == 'types' else 0, REGISTERS[reg][0], 0]))
        data = operating_byte(raw) if self.family == 'ACR122U' else escape_payload(raw)
        require(len(data) == REGISTERS[reg][1] or reg == 'pps' and len(data) == 2, 'Unexpected reader register length.')
        return data

    async def read(self):
        snapshot = {'reader': self.reader, 'family': self.family, 'firmware': await self.firmware(), 'values': {}, 'errors': {}, 'serial': None}
        if self.family == 'ACR1552U':
            try:
                snapshot['serial'] = escape_payload(await self.control(bytes.fromhex('E000003300')))
            except Exception:
                pass
        regs = ['operating'] if self.family == 'ACR122U' else ['polling', 'types', 'pps', 'ui', 'hid', 'characters', 'keyboard', 'host', 'power', 'suspend']
        for reg in regs:
            if reg in ('power', 'suspend') and not supports(snapshot, reg):
                continue
            try:
                snapshot['values'][reg] = await self.read_register(reg)
            except Exception as exc:
                snapshot['errors'][reg] = str(exc)
        return snapshot

    async def same_reader(self, snapshot):
        require(snapshot['reader'] == self.reader and await self.firmware() == snapshot['firmware'], 'Reader identity changed; read settings again.')
        if snapshot['serial'] is not None:
            require(escape_payload(await self.control(bytes.fromhex('E000003300'))) == snapshot['serial'], 'Reader serial changed.')

    async def apply(self, snapshot, edits):
        changes = plan(snapshot, edits)
        await self.same_reader(snapshot)
        regs = {r for r, _, _ in changes if r not in WRITE_ONLY}
        if regs & {'host', 'suspend'}:
            regs |= {'host', 'suspend'} & snapshot['values'].keys()
        for reg in regs:
            require((await self.read_register(reg))[:REGISTERS[reg][1]] == snapshot['values'][reg][:REGISTERS[reg][1]], 'Reader settings changed; read them again.')
        results = []
        for reg, value, title in changes:
            try:
                if self.family == 'ACR122U':
                    command = bytes([255, 0, 0x51 if reg == 'operating' else 0x52, value[0], 0]) if reg != 'rf' else bytes([255, 0, 0, 0, 4, 0xD4, 0x32, 1, value[0]])
                else:
                    payload = b'\0'+value if reg == 'usbSerial' else value
                    command = bytes([0xE0, 0, 1 if reg == 'types' else 0, REGISTERS[reg][0], len(payload)])+payload
                error = None
                try:
                    raw = await self.control(command)
                    if self.family == 'ACR1552U':
                        require(escape_payload(raw) == value+(b'\x90\0' if reg == 'usbSerial' else b''), 'Setting was not acknowledged.')
                    elif reg == 'operating':
                        require(operating_byte(raw) == value, 'Polling value was not acknowledged.')
                    else:
                        require(raw == (b'\xd5\x33\x90\0' if reg == 'rf' else b'\x90\0'), 'Command was not acknowledged.')
                except Exception as exc:
                    error = exc
                if reg == 'power':
                    verified = False
                    for _ in range(20):
                        await asyncio.sleep(.25)
                        try:
                            if await self.read_register(reg) == value:
                                await self.same_reader(snapshot)
                                verified = True
                                break
                        except Exception:
                            continue
                    require(verified, 'Power command sent once but not verified after restart. Reconnect and read settings.')
                else:
                    if error:
                        raise error
                    if reg not in WRITE_ONLY:
                        require((await self.read_register(reg))[:len(value)] == value, 'Setting did not match on read-back.')
                results.append(title + (' · acknowledged (no read-back command)' if reg in WRITE_ONLY else ' · verified')
                               + (' · unplug/reconnect to activate' if reg in ('polling', 'types', 'ui', 'usbSerial') else ''))
            except Exception as exc:
                raise CardError('\n'.join(results+[f'{title}: not verified. {exc} Earlier changes may be applied; remaining changes were not sent.'])) from exc
        return '\n'.join(results) or 'No settings changed.'
