import pytest
from quadband import chameleon, t5577
from quadband.common import CardError


@pytest.mark.parametrize('format,data,config', [
    ('EM410x', bytes.fromhex('0102030405'), 0x00148040),
    ('EM410x Electra', bytes(range(13)), 0x00148080),
    ('Viking', bytes([1,2,3,4]), 0x00088040),
    ('ioProx', bytes(range(16)), 0x00147040),
    ('PAC / Stanley', b'12345678', 0x00080080),
    ('Jablotron', bytes(range(5)), 0x00158040)])
def test_encoders_never_enable_password_or_write_password_block(format, data, config):
    words = t5577.blocks(chameleon.LFCard(format, data))
    assert words[-1] == (0, config) and config & 0x10 == 0
    for block, word in words:
        payload = t5577.payload(block, word)
        assert block < 7 and payload[5:] == bytes(6)


def test_em410x_row_and_column_parity():
    data = bytes.fromhex('0102030405')
    words = t5577.blocks(chameleon.LFCard('EM410x', data))
    raw = ''.join(f'{word:032b}' for b, word in words if b)
    assert len(raw) == 64 and raw[:9] == '1'*9 and raw[-1] == '0'
    rows = [raw[9+i*5:14+i*5] for i in range(10)]
    assert all(row.count('1') % 2 == 0 for row in rows)
    assert bytes(int(rows[i][:4]+rows[i+1][:4], 2) for i in range(0, 10, 2)) == data
    assert all((sum(int(r[i]) for r in rows)+int(raw[59+i])) % 2 == 0 for i in range(4))


@pytest.mark.parametrize('data', [bytes([7])+bytes(10), bytes([0])+bytes.fromhex('00148050')+bytes(6),
                                  bytes([1])+bytes(4)+b'\1'+bytes(5), bytes([1])+bytes(9)+b'\1'])
def test_guard_rejects_password_block_pwd_configuration_authentication_and_page1(data):
    with pytest.raises(CardError):
        t5577.guard(3016, data)


async def test_legacy_writer_commands_blocked_before_transport():
    class Transport:
        async def exchange(self, *_):
            raise AssertionError('Password command reached hardware')
    client = chameleon.Chameleon(Transport())
    for command in t5577.LEGACY_PASSWORD_WRITERS:
        with pytest.raises(CardError, match='disabled'):
            await client.command(command)
    client.capabilities = {1001, 3000, 3001}
    client.original_mode = b'\0'
    with pytest.raises(CardError, match='3016'):
        await client.write(chameleon.LFCard('EM410x', bytes(5)))
