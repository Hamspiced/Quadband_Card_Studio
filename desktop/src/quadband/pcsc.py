"""PC/SC through Windows WinSCard / Linux pcsc-lite. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import sys
from contextlib import asynccontextmanager
from .common import CardError, NoCard, AuthenticationError, require
from .hf import CardInfo, ClassicClient, data_block, sector_of


def readers():
    from smartcard.System import readers as list_readers
    return [str(r) for r in list_readers() if 'SAM' not in str(r).upper() and 'CONTACT READER' not in str(r).upper()]


def find_reader(name):
    from smartcard.System import readers as list_readers
    selected = next((r for r in list_readers() if str(r) == name), None)
    require(selected is not None, 'The selected reader is unavailable. Refresh readers and reconnect.')
    return selected


GONE = {0x8010000C, 0x80100069}


def removed(exc):
    value = getattr(exc, 'hresult', None)
    return isinstance(value, int) and value & 0xFFFFFFFF in GONE


class PCSC:
    def __init__(self, name):
        self.name, self.connection, self.transaction = name, None, False
        self.card = None
        self.previous = {}
        self.authentication = b'\0'
        self.read_auth = None

    def open(self):
        from smartcard.Exceptions import NoCardException
        from smartcard.ExclusiveTransmitCardConnection import ExclusiveTransmitCardConnection
        if self.connection:
            return
        connection = ExclusiveTransmitCardConnection(find_reader(self.name).createConnection())
        try:
            connection.connect()
            connection.lock()
        except NoCardException as exc:
            connection.disconnect()
            raise NoCard('Place one HF card on the reader.') from exc
        except Exception as exc:
            connection.disconnect()
            if removed(exc):
                raise NoCard('Place one HF card on the reader.') from exc
            raise
        self.connection, self.transaction = connection, True

    async def close(self):
        self.read_auth = None
        self.card = None
        self.previous.clear()
        if self.connection:
            connection, self.connection = self.connection, None
            try:
                if self.transaction:
                    connection.unlock()
            finally:
                self.transaction = False
                connection.disconnect()

    async def present(self):
        try:
            await self.identify()
            return True
        except NoCard:
            return False

    async def command(self, command, count=None):
        from smartcard.Exceptions import NoCardException
        self.open()
        try:
            payload, sw1, sw2 = self.connection.transmit(list(command))
        except NoCardException as exc:
            raise NoCard('The card was removed.') from exc
        except Exception as exc:
            if removed(exc):
                raise NoCard('The card was removed.') from exc
            raise
        await asyncio.sleep(0)
        status = (sw1 << 8) | sw2
        if status in (0x6300, 0x6982, 0x6985):
            raise AuthenticationError(f'Card access failed ({status:04X}); check key, permissions and placement.')
        require(status == 0x9000, f'Card command failed ({status:04X}).')
        data = bytes(payload)
        require(count is None or len(data) == count, 'Incomplete card response.')
        return data

    async def identify(self):
        self.read_auth = None
        uid = await self.command(bytes.fromhex('FFCA000000'))
        require(len(uid) in (4, 7, 8, 10), 'Unsupported card ID length.')
        card = CardInfo(uid, bytes(self.connection.getATR()))
        if not card.classic and len(uid) == 7:
            from .formatting import VERSIONS
            version = await self.get_version()
            family = next((name for name, value in VERSIONS.items() if value == version), None)
            if family:
                card = CardInfo(uid, card.atr, family_hint=family, version=version)
        if card != self.card:
            self.previous.clear()
        self.card = card
        return card

    async def get_version(self):
        wrapped, raw = bytes.fromhex('FF00000004D4400160'), bytes.fromhex('FF0000000160')
        for apdu in ([wrapped, raw] if 'ACR122' in self.name.upper() else [raw, wrapped]):
            try:
                data = await self.command(apdu)
            except NoCard:
                raise
            except CardError:
                continue
            if data[:3] == b'\xd5\x41\x00':
                data = data[3:]
            if len(data) == 8:
                return data
        return None

    async def read_pages(self, page, count=1):
        card = self.card
        require(card and card.ntag and 1 <= count <= 16
                and 0 <= page < page+count <= card.unit_count - 2, 'Invalid readable NTAG page range.')
        result = b''
        for offset in range(0, count, 4):
            amount = min(4, count-offset)
            start = min(page+offset, card.unit_count-6)
            data = await self.command(bytes([255, 0xB0, 0, start, 16]), 16)
            result += data[(page+offset-start)*4:(page+offset-start+amount)*4]
        require(len(result) == count*4, 'Incomplete NTAG page response.')
        for offset in range(count):
            self.previous[page+offset] = result[offset*4:offset*4+4]
        return result

    async def write_page(self, page, data):
        from .hf import Snapshot
        from .ntag import writable
        card = await self.identify()
        require(card.ntag and len(data) == 4 and page in self.previous, 'Read an identified NTAG page first.')
        snapshot = Snapshot(card, dict(self.previous))
        dynamic = card.unit_count-5
        for unit in (2, 3, dynamic, dynamic+1, dynamic+2):
            snapshot.blocks[unit] = await self.read_pages(unit)
        require(writable(snapshot, page), 'NTAG page is locked or protected.')
        previous = self.previous[page]
        require(await self.read_pages(page) == previous, 'NTAG page changed since read.')
        await self.command(bytes([255, 0xD6, 0, page, 4])+data, 0)
        require(await self.read_pages(page) == data and await self.identify() == card, 'NTAG write unverified.')

    async def canonical_format(self, snapshot, overwrite=False):
        from .formatting import canonical
        async with asyncio.timeout(120):
            await canonical(self, snapshot, overwrite)

    async def _format_unit(self, unit, data, credential):
        from .formatting import canonical_unit
        canonical_unit(self.card, unit, data, credential)
        if credential is not None:
            await self.authenticate(unit, credential)
        await self.command(bytes([255, 0xD6, 0, unit, len(data)])+data, 0)

    async def authenticate(self, block, credential):
        key, kind = credential
        require(len(key) == 6 and kind in (0x60, 0x61) and 0 <= block < 256, 'Invalid Classic block or key.')
        await self.command(b'\xff\x82\0\0\6'+key, 0)
        await self.command(bytes([255, 0x86, 0, 0, 5, 1, 0, block, kind, 0]), 0)

    async def read_block(self, block, credential):
        token = (sector_of(block), credential)
        try:
            if self.read_auth != token:
                await self.authenticate(block, credential)
                self.read_auth = token
            return await self.command(bytes([255, 0xB0, 0, block, 16]), 16)
        except BaseException:
            self.read_auth = None
            raise

    async def write_block(self, block, data, credential):
        card = await self.identify()
        require(card.classic and data_block(block, card.unit_count) and len(data) == 16, 'Only 16-byte Classic data blocks can be written.')
        await self.authenticate(block, credential)
        await self.command(bytes([255, 0xD6, 0, block, 16])+data, 0)

    async def reset_authentication(self):
        self.read_auth = None
        # Reconnecting with a reset revives Classic cards halted by a failed key.
        from smartcard.scard import SCARD_RESET_CARD
        if self.connection:
            self.connection.unlock()
            self.transaction = False
            self.connection.reconnect(disposition=SCARD_RESET_CARD)
            self.connection.lock()
            self.transaction = True
        await asyncio.sleep(0)


@asynccontextmanager
async def session(name, chameleon=None):
    if name == 'Chameleon Ultra · connected':
        require(chameleon is not None and not chameleon.transport.closed, 'Connect the Chameleon in LF Workspace first.')
        async with chameleon.lease():
            yield ClassicClient(chameleon)
    else:
        backend = PCSC(name)
        try:
            from .cores3 import HFClient
            yield HFClient(backend)
        finally:
            await backend.close()


class ReaderControl:
    """Short-lived handles allow recovery after an ACR1552 power-setting reset."""
    def __init__(self, name):
        self.name = name

    async def control(self, command):
        from smartcard.scard import SCARD_SHARE_DIRECT, SCARD_PROTOCOL_UNDEFINED
        from smartcard.ExclusiveTransmitCardConnection import ExclusiveTransmitCardConnection
        connection = find_reader(self.name).createConnection()
        transaction, direct = False, True
        try:
            try:
                connection.connect(mode=SCARD_SHARE_DIRECT, protocol=SCARD_PROTOCOL_UNDEFINED)
            except Exception:
                connection.disconnect()
                connection = ExclusiveTransmitCardConnection(find_reader(self.name).createConnection())
                connection.connect()
                connection.lock()
                transaction, direct = True, False
            if not direct and command[0] == 255:
                data, sw1, sw2 = connection.transmit(list(command))
                reply = bytes(data+[sw1, sw2])
            else:
                # Windows SCARD_CTL_CODE differs from pcsc-lite/macOS.
                code = (0x31 << 16) | (3500 << 2) if sys.platform == 'win32' else 0x42000000+3500
                reply = bytes(connection.control(code, list(command)))
            await asyncio.sleep(0)
            return reply
        finally:
            try:
                if transaction:
                    connection.unlock()
            finally:
                connection.disconnect()
