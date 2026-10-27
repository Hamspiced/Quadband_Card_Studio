import pytest
from quadband import reader_tools as rt
from quadband.common import CardError


def snapshot(family='ACR1552U', firmware='ACR1552U-M 1.06.00'):
    return {'reader': family, 'family': family, 'firmware': firmware, 'serial': b'1234',
            'values': {'polling': b'\x8b', 'ui': b'\xD7', 'power': b'\0', 'host': b'\1',
                       'suspend': b'\0', 'operating': b'\xff', 'types': b'\x07\x05'}, 'errors': {}}


def test_buzzer_preserves_unrelated_bits():
    s = snapshot()
    changes = rt.plan(s, rt.preset(s, 'mute'))
    assert changes[0][0:2] == ('ui', b'\xC7')
    assert rt.plan(snapshot('ACR122U'), {'buzzer': 0})[0][0:2] == ('buzzer', b'\0')


def test_presets_and_firmware_gates():
    s = snapshot()
    changes = rt.plan(s, rt.preset(s, 'ashsh'))
    assert [(r, v) for r, v, _ in changes] == [('polling', b'\x89'), ('power', b'\5')]
    with pytest.raises(CardError):
        rt.plan(snapshot(firmware='ACR1552U-M 1.02.00'), {'power': 4})
    with pytest.raises(CardError):
        rt.plan(s, {'suspend': 1, 'host': 2})


class Control:
    def __init__(self):
        self.snapshot = snapshot()
        self.writes = []
        self.stale = False
    async def __call__(self, command):
        address, count = command[3], command[4]
        if address == 0x18:
            data = self.snapshot['firmware'].encode()
        elif address == 0x33:
            data = b'1234'
        else:
            reg = next(reg for reg, (addr, _) in rt.REGISTERS.items() if addr == address)
            if count:
                self.writes.append((reg, command[5:]))
                self.snapshot['values'][reg] = command[5:]
            data = self.snapshot['values'].get(reg, bytes(rt.REGISTERS[reg][1]))
            if self.stale and reg == 'ui':
                data = b'\0'
        return b'\xe1\0\0\0'+bytes([len(data)])+data


async def test_settings_stale_preflight_writes_nothing():
    transport = Control()
    client = rt.ACS(transport, 'ACR1552U')
    s = await client.read()
    transport.stale = True
    with pytest.raises(CardError, match='changed'):
        await client.apply(s, {'presenceBeep': 8})
    assert not transport.writes


async def test_settings_apply_ack_and_readback():
    transport = Control()
    client = rt.ACS(transport, 'ACR1552U')
    s = await client.read()
    result = await client.apply(s, {'presenceBeep': 8})
    assert 'verified' in result
    assert transport.writes == [('ui', b'\xDF')]
