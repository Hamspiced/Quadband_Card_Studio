"""Opt-in read-only checks. Never writes a physical card.

QUADBAND_PORTABLE_ACR_READER: exact PC/SC reader name.
QUADBAND_PORTABLE_CHAMELEON_PORT: selected Chameleon serial path / COM port.
Do not set these variables in ordinary CI.
"""
import os
import pytest
from quadband import pcsc
from quadband.service import Service


@pytest.mark.skipif(not os.getenv('QUADBAND_PORTABLE_ACR_READER'), reason='Physical PC/SC reader is opt-in')
async def test_acr_identification_and_memory_read():
    async with pcsc.session(os.environ['QUADBAND_PORTABLE_ACR_READER']) as client:
        info = await client.backend.identify()
        assert info.uid
        if info.classic:
            snapshot = await client.read()
            assert snapshot.blocks, 'No blocks could be read with the configured standard keys.'


@pytest.mark.skipif(not os.getenv('QUADBAND_PORTABLE_CHAMELEON_PORT'), reason='Physical Chameleon is opt-in')
async def test_chameleon_read_only_identity_capabilities():
    service = Service(lambda _: None)
    try:
        label = await service.connect('chameleon', 'USB serial', os.environ['QUADBAND_PORTABLE_CHAMELEON_PORT'], 115200)
        assert 'Chameleon Ultra' in label
        assert {1001, 1002} <= service.chameleon.capabilities
        print('Open T5577 block-write available:', 3016 in service.chameleon.capabilities)
        # No scan, card write or mode change is performed by identification.
    finally:
        if service.chameleon:
            await service.chameleon.transport.close()
