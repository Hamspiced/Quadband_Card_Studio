"""NDEF codecs and record builders. SPDX-License-Identifier: GPL-3.0-only"""
from dataclasses import dataclass
import json
import math
import re
from urllib.parse import urlsplit, urlencode, quote
from .common import CardError, require, hex_bytes

PREFIXES = ['', 'http://www.', 'https://www.', 'http://', 'https://', 'tel:', 'mailto:',
            'ftp://anonymous:anonymous@', 'ftp://ftp.', 'ftps://', 'sftp://', 'smb://', 'nfs://',
            'ftp://', 'dav://', 'news:', 'telnet://', 'imap:', 'rtsp://', 'urn:', 'pop:', 'sip:',
            'sips:', 'tftp:', 'btspp://', 'btl2cap://', 'btgoep://', 'tcpobex://', 'irdaobex://',
            'file://', 'urn:epc:id:', 'urn:epc:tag:', 'urn:epc:pat:', 'urn:epc:raw:', 'urn:epc:', 'urn:nfc:']


@dataclass(frozen=True)
class Record:
    tnf: int
    type: bytes
    payload: bytes
    identifier: bytes = b''

    @classmethod
    def uri(cls, uri):
        match = max((i for i, p in enumerate(PREFIXES) if uri.startswith(p)), key=lambda i: len(PREFIXES[i]))
        return cls(1, b'U', bytes([match])+uri[len(PREFIXES[match]):].encode())

    @classmethod
    def text(cls, text, language='en'):
        require(re.fullmatch('[A-Za-z-]{1,63}', language), 'Use a language code such as en or en-US.')
        return cls(1, b'T', bytes([len(language)])+language.encode()+text.encode())

    def display(self):
        try:
            if self.tnf == 1 and self.type == b'U' and self.payload and self.payload[0] < len(PREFIXES):
                return 'URL / URI', PREFIXES[self.payload[0]] + self.payload[1:].decode()
            if self.tnf == 1 and self.type == b'T' and self.payload and not self.payload[0] & 64:
                start = 1+(self.payload[0] & 63)
                require(start <= len(self.payload), 'Truncated text.')
                return 'Text', self.payload[start:].decode('utf-16' if self.payload[0] & 128 else 'utf-8')
            if self.type == b'application/vnd.wfa.wsc':
                credential = dict(wsc_fields(self.payload)).get(0x100E, b'')
                name = dict(wsc_fields(credential)).get(0x1045, b'').decode()
                return 'Wi-Fi network', f'{name}\nConnection settings are stored on the tag.'
            if self.type == b'application/vnd.bluetooth.ep.oob' and len(self.payload) >= 8:
                return 'Bluetooth', self.payload[2:8][::-1].hex(':').upper()
            text = self.payload.decode()
            if any(ord(c) < 32 and c not in '\r\n\t' for c in text):
                text = self.payload.hex(' ').upper()
            return self.type.decode(errors='replace') or 'Data', text
        except (UnicodeError, CardError, ValueError):
            return 'Data', self.payload.hex(' ').upper()


def validate_record(r):
    require(0 <= r.tnf <= 5 and len(r.type) <= 255 and len(r.identifier) <= 255 and len(r.payload) <= 1048576,
            'Invalid NDEF record size or type.')
    require(not (r.tnf == 0 and (r.type or r.identifier or r.payload)) and
            not (r.tnf == 5 and r.type) and not (0 < r.tnf < 5 and not r.type), 'Invalid NDEF type.')


def encode(records):
    require(1 <= len(records) <= 64, 'Add between 1 and 64 records.')
    out = b''
    for i, r in enumerate(records):
        validate_record(r)
        short = len(r.payload) < 256
        header = r.tnf | (128 if i == 0 else 0) | (64 if i == len(records)-1 else 0) | (16 if short else 0) | (8 if r.identifier else 0)
        out += bytes([header, len(r.type)]) + len(r.payload).to_bytes(1 if short else 4, 'big')
        out += (bytes([len(r.identifier)]) if r.identifier else b'') + r.type + r.identifier + r.payload
    require(len(out) <= 1048576, 'NDEF message too large.')
    return out


def decode(data):
    require(len(data) <= 1048576, 'NDEF message too large.')
    records, cursor, ended = [], 0, False
    def take(n):
        nonlocal cursor
        require(0 <= n <= len(data)-cursor, 'Truncated NDEF record.')
        result = data[cursor:cursor+n]
        cursor += n
        return result
    while cursor < len(data):
        header, type_len = take(2)
        require(bool(header & 128) == (not records) and not header & 32 and not ended,
                'Unsupported chunked or malformed NDEF message.')
        length = int.from_bytes(take(1 if header & 16 else 4), 'big')
        id_len = take(1)[0] if header & 8 else 0
        type, identifier, payload = take(type_len), take(id_len), take(length)
        record = Record(header & 7, type, payload, identifier)
        validate_record(record)
        records.append(record)
        require(len(records) <= 64, 'Too many NDEF records.')
        ended = bool(header & 64)
    require(ended or not data, 'Missing NDEF end marker.')
    return records


# id, label, default, choices; the same catalog drives forms and tests.
FIELDS = {
    'Text': [('text', 'Text', '', []), ('language', 'Language', 'en', [])],
    **{kind: [('url', 'Complete link / URI', '', [])] for kind in
       ['URL / URI', 'Custom URL / URI', 'Unit.Link', 'Payment Link', 'Social Networks', 'Video Link', 'File Link']},
    'Search': [('query', 'Search terms', '', []), ('engine', 'Search engine', 'Google', ['Google', 'DuckDuckGo', 'Bing'])],
    'Application': [('package', 'Android package name', '', [])],
    'Mail': [('email', 'Email', '', []), ('subject', 'Subject', '', []), ('body', 'Message', '', [])],
    'Contact': [(k, k.title(), '', []) for k in ['name', 'organization', 'phone', 'email', 'url']],
    'Phone': [('phone', 'Phone number', '', [])],
    'Location': [('latitude', 'Latitude', '', []), ('longitude', 'Longitude', '', [])],
    'SOS': [(k, k.title(), '', []) for k in ['name', 'contact', 'phone', 'medical', 'notes']],
    'Bluetooth': [('address', 'Bluetooth address', '', []), ('name', 'Device name', '', [])],
    '3D Printing': [('material', 'Material', 'PLA', []), ('brand', 'Brand', 'Generic', []),
                    ('color', 'Color hex', 'FFFFFF', []), ('minTemp', 'Minimum °C', '190', []), ('maxTemp', 'Maximum °C', '220', [])],
    'Wi-Fi': [('ssid', 'Network name', '', []), ('security', 'Security', 'WPA2 Personal', ['WPA2 Personal', 'Open']),
              ('password', 'Network password (stored on tag)', '', [])],
    'Data': [('mime', 'MIME type', 'application/octet-stream', []),
             ('encoding', 'Payload format', 'Hexadecimal', ['Hexadecimal', 'UTF-8 text']), ('payload', 'Data', '', [])]
}


def wsc_attribute(kind, value):
    return kind.to_bytes(2, 'big') + len(value).to_bytes(2, 'big') + value


def wsc_fields(data):
    fields = []
    while data:
        require(len(data) >= 4, 'Truncated WSC attribute.')
        kind, length = int.from_bytes(data[:2], 'big'), int.from_bytes(data[2:4], 'big')
        require(length <= len(data)-4, 'Truncated WSC value.')
        fields.append((kind, data[4:4+length]))
        data = data[4+length:]
    return fields


def build(kind, values):
    require(kind in FIELDS, 'Unknown record type.')
    v = {key: values.get(key, default) for key, _, default, _ in FIELDS[kind]}
    require(all(len(s.encode()) <= 8192 for s in v.values()), 'Record field is too long.')
    def required(key):
        text = v[key].strip()
        require(bool(text), f'Enter {key}.')
        return text
    if kind == 'Text':
        required('text')
        return Record.text(v['text'], required('language'))
    if 'url' in v and kind != 'Contact':
        uri = required('url')
        parsed = urlsplit(uri)
        require(bool(parsed.scheme) and not any(c.isspace() for c in uri), 'Enter a complete URI including its scheme.')
        if kind not in ('Custom URL / URI', 'Payment Link', 'File Link'):
            require(parsed.scheme.lower() in ('https', 'http') and bool(parsed.hostname), 'Use a complete http:// or https:// link.')
        return Record.uri(uri)
    if kind == 'Search':
        base = {'Google': 'https://www.google.com/search', 'DuckDuckGo': 'https://duckduckgo.com/', 'Bing': 'https://www.bing.com/search'}[v['engine']]
        return Record.uri(base+'?'+urlencode({'q': required('query')}, quote_via=quote))
    if kind == 'Application':
        package = required('package')
        require(re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+', package), 'Enter an Android package name such as com.example.app.')
        return Record(4, b'android.com:pkg', package.encode())
    if kind == 'Mail':
        email = required('email')
        require('@' in email and not re.search(r'[\s?#]', email), 'Enter an email address.')
        query = urlencode({k: v[k] for k in ('subject', 'body') if v[k]}, quote_via=quote)
        return Record.uri('mailto:'+quote(email, safe='@.+')+('?' + query if query else ''))
    if kind == 'Phone':
        phone = re.sub(r'\s', '', required('phone'))
        require(re.fullmatch(r'\+?[0-9()\-]+', phone), 'Enter a phone number with an optional country code.')
        return Record.uri('tel:'+phone)
    if kind == 'Location':
        try:
            lat, lon = float(v['latitude']), float(v['longitude'])
        except ValueError as exc:
            raise CardError('Enter numeric latitude and longitude.') from exc
        require(math.isfinite(lat) and math.isfinite(lon) and -90 <= lat <= 90 and -180 <= lon <= 180, 'Coordinates are out of range.')
        return Record.uri(f'geo:{lat},{lon}')
    if kind == 'Contact':
        name = required('name')
        def escape(text):
            return text.replace('\\', '\\\\').replace('\r\n', '\n').replace('\r', '\n').replace('\n', '\\n').replace(';', '\\;').replace(',', '\\,')
        lines = ['BEGIN:VCARD', 'VERSION:3.0', 'N:;'+escape(name)+';;;', 'FN:'+escape(name)]
        lines += [label+':'+escape(v[k]) for k, label in [('organization', 'ORG'), ('phone', 'TEL'), ('email', 'EMAIL'), ('url', 'URL')] if v[k]]
        lines += ['END:VCARD']
        def fold(line):
            result, size = '', 0
            for char in line:
                n = len(char.encode())
                if size+n > 75:
                    result += '\r\n '
                    size = 1
                result += char
                size += n
            return result
        return Record(2, b'text/vcard', ('\r\n'.join(map(fold, lines))+'\r\n').encode())
    if kind == 'SOS':
        text = 'EMERGENCY INFORMATION\nName: '+required('name')
        text += ''.join('\n'+k.title()+': '+v[k] for k in ('contact', 'phone', 'medical', 'notes') if v[k])
        return Record.text(text)
    if kind == 'Bluetooth':
        address, name = hex_bytes(required('address'), 6), v['name'].encode()
        require(len(name) <= 248, 'Bluetooth name is too long.')
        extra = bytes([len(name)+1, 9])+name if name else b''
        return Record(2, b'application/vnd.bluetooth.ep.oob', (8+len(extra)).to_bytes(2, 'little')+address[::-1]+extra)
    if kind == '3D Printing':
        color = v['color'].strip('#').upper()
        hex_bytes(color, 3)
        try:
            low, high = int(v['minTemp']), int(v['maxTemp'])
        except ValueError as exc:
            raise CardError('Enter whole-number temperatures.') from exc
        require(0 <= low <= high <= 500, 'Temperature limits must be 0–500 °C.')
        obj = {'protocol': 'openspool', 'version': '1.0', 'type': required('material'), 'brand': required('brand'),
               'color_hex': color, 'min_temp': str(low), 'max_temp': str(high)}
        return Record(2, b'application/json', json.dumps(obj, sort_keys=True, separators=(',', ':')).encode())
    if kind == 'Wi-Fi':
        ssid, password, opened = v['ssid'].encode(), v['password'].encode(), v['security'] == 'Open'
        require(0 < len(ssid) <= 32, 'SSID must contain 1–32 UTF-8 bytes.')
        require(opened or 8 <= len(password) <= 63 or (len(password) == 64 and re.fullmatch('[0-9A-Fa-f]{64}', v['password'])),
                'WPA2 password needs 8–63 bytes or 64 hexadecimal digits.')
        credential = b''.join(wsc_attribute(k, data) for k, data in [
            (0x1026, b'\1'), (0x1045, ssid), (0x1003, b'\0\1' if opened else b'\0\x20'),
            (0x100F, b'\0\1' if opened else b'\0\x08'), (0x1027, b'' if opened else password), (0x1020, b'\xff'*6)])
        return Record(2, b'application/vnd.wfa.wsc', wsc_attribute(0x104A, b'\x10')+wsc_attribute(0x100E, credential))
    mime = required('mime').lower()
    require(re.fullmatch(r'[a-z0-9!#$&^_.+\-]+/[a-z0-9!#$&^_.+\-]+', mime), 'Enter a MIME type such as application/json.')
    return Record(2, mime.encode(), v['payload'].encode() if v['encoding'] == 'UTF-8 text' else hex_bytes(v['payload']))
