"""PC/SC through Windows WinSCard / Linux pcsc-lite. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import sys
from contextlib import asynccontextmanager
from .common import CardError, NoCard, AuthenticationError, require
from .hf import CardInfo, ClassicClient, data_block


def readers():
    from smartcard.System import readers as list_readers
    return [str(r) for r in list_readers() if 'SAM' not in str(r).upper() and 'CONTACT READER' not in str(r).upper()]


def find_reader(name):
    from smartcard.System import readers as list_readers
    selected = next((r for r in list_readers() if str(r) == name), None)
    require(selected is not None, 'The selected reader is unavailable. Refresh readers and reconnect.')
    return selected


# SCARD_W_UNRESPONSIVE_CARD, SCARD_W_UNPOWERED_CARD, SCARD_W_REMOVED_CARD: what the reader reports while a tag
# is leaving the field. The tag cannot be used, so it counts as no card rather than as a failure.
GONE = {0x80100066, 0x80100067, 0x80100069}


class PCSC:
    def __init__(self, name):
        self.name, self.connection, self.transaction = name, None, False

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
            if getattr(exc, 'hresult', -1) & 0xFFFFFFFF in GONE:
                raise NoCard('Place one HF card on the reader.') from exc
            raise
        self.connection, self.transaction = connection, True

    async def close(self):
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
        await asyncio.sleep(0)
        status = (sw1 << 8) | sw2
        if status in (0x6300, 0x6982, 0x6985):
            raise AuthenticationError(f'Card access failed ({status:04X}); check key, permissions and placement.')
        require(status == 0x9000, f'Card command failed ({status:04X}).')
        data = bytes(payload)
        require(count is None or len(data) == count, 'Incomplete card response.')
        return data

    async def identify(self):
        uid = await self.command(bytes.fromhex('FFCA000000'))
        require(len(uid) in (4, 7, 8, 10), 'Unsupported card ID length.')
        return CardInfo(uid, bytes(self.connection.getATR()))

    async def authenticate(self, block, credential):
        key, kind = credential
        require(len(key) == 6 and kind in (0x60, 0x61) and 0 <= block < 64, 'Invalid Classic block or key.')
        await self.command(b'\xff\x82\0\0\6'+key, 0)
        await self.command(bytes([255, 0x86, 0, 0, 5, 1, 0, block, kind, 0]), 0)

    async def read_block(self, block, credential):
        await self.authenticate(block, credential)
        return await self.command(bytes([255, 0xB0, 0, block, 16]), 16)

    async def write_block(self, block, data, credential):
        require(data_block(block) and len(data) == 16, 'Only 16-byte Classic data blocks can be written.')
        await self.authenticate(block, credential)
        await self.command(bytes([255, 0xD6, 0, block, 16])+data, 0)

    async def write_trailer(self, block, data, credential):
        # Only used by NFC formatting of blank factory-key cards; callers verify the result.
        require(block in range(3, 64, 4) and len(data) == 16, 'A sector trailer is exactly 16 bytes.')
        await self.authenticate(block, credential)
        await self.command(bytes([255, 0xD6, 0, block, 16])+data, 0)

    async def read_pages(self, page):
        # NFC Forum Type 2 (NTAG): four 4-byte pages per read; no authentication.
        require(0 <= page < 256, 'Invalid tag page.')
        return await self.command(bytes([255, 0xB0, 0, page, 16]), 16)

    async def write_page(self, page, data):
        # Pages 0-2 hold the UID and the permanent lock bytes and are never written.
        require(3 <= page < 256 and len(data) == 4, 'Only 4-byte tag pages from page 3 can be written.')
        await self.command(bytes([255, 0xD6, 0, page, 4])+data, 0)

    async def get_version(self):
        """NTAG GET_VERSION (60h), or None when this reader will not pass the command through."""
        wrapped, raw = bytes.fromhex('FF00000004D4400160'), bytes.fromhex('FF0000000160')
        for apdu in ([wrapped, raw] if 'ACR122' in self.name.upper() else [raw, wrapped]):
            data = b''
            try:
                data = await self.command(apdu)
            except NoCard:
                raise
            except CardError:
                pass
            if data[:3] == b'\xd5\x41\x00':
                data = data[3:]
            if len(data) == 8 and data[:3] == b'\0\4\4':
                return data
            await self.reset_authentication()
        return None

    async def reset_authentication(self):
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
        yield ClassicClient(chameleon)
    else:
        backend = PCSC(name)
        try:
            yield ClassicClient(backend)
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
