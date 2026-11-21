"""Generate portable test vectors from the existing desktop codecs, without hardware."""
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "desktop/src"))
from quadband import ndef, hf, ntag

examples = {
    "Text": {"text": "Hello NFC", "language": "en-US"},
    **{kind: {"url": "https://www.example.com/card?a=1"} for kind in [
        "URL / URI", "Custom URL / URI", "Unit.Link", "Payment Link",
        "Social Networks", "Video Link", "File Link"]},
    "Search": {"query": "NFC test & card", "engine": "DuckDuckGo"},
    "Application": {"package": "com.example.card"},
    "Mail": {"email": "test+card@example.com", "subject": "Hello NFC", "body": "a&b"},
    "Contact": {"name": "Example; Person", "organization": "Company", "phone": "+1234", "email": "a@b.com"},
    "Phone": {"phone": "+1 (234) 567-890"},
    "Location": {"latitude": "41.5", "longitude": "-87.25"},
    "SOS": {"name": "Example", "contact": "Family", "medical": "None", "notes": "Test only"},
    "Bluetooth": {"address": "01:23:45:67:89:AB", "name": "Headset"},
    "3D Printing": {"material": "PLA", "brand": "Generic", "color": "#ABCDEF"},
    "Wi-Fi": {"ssid": "Test network", "password": "example123"},
    "Data": {"mime": "application/octet-stream", "payload": "00 01 FE FF"},
}

def cpp_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + '"'

out = ["#pragma once", "struct Golden { size_t kind; NdefValues values; Bytes encoded; };",
       "static const std::vector<Golden> goldens = {"]
for i, (kind, values) in enumerate(examples.items()):
    encoded = ndef.encode([ndef.build(kind, values)])
    pairs = ",".join("{" + cpp_string(k) + "," + cpp_string(v) + "}" for k, v in values.items())
    out.append("{" + str(i) + ",{" + pairs + "},{" + ",".join(str(b) for b in encoded) + "}},")
for kind, values in [
    ("3D Printing", {"brand": "Café 🧵", "material": "PLA\tEco"}),
    ("Contact", {"name": "é"*60, "organization": "A\r\nB,C"}),
    ("Location", {"latitude": "41.123456789012345", "longitude": "-0.0000123456789"}),
    ("Location", {"latitude": "0.0001", "longitude": "-0"}),
]:
    encoded = ndef.encode([ndef.build(kind, values)])
    pairs = ",".join("{" + cpp_string(k) + "," + cpp_string(v) + "}" for k, v in values.items())
    out.append("{" + str(list(ndef.FIELDS).index(kind)) + ",{" + pairs + "},{" + ",".join(str(b) for b in encoded) + "}},")
out += ["};"]
mad1 = bytearray(32)
mad1[2:4] = b"\x03\xe1"
mad2 = bytearray(48)
mad2[32:34] = b"\x03\xe1"
out += [f"static constexpr uint8_t golden_mad1_crc = {hf.mad_crc(mad1[1:])};",
        f"static constexpr uint8_t golden_mad2_crc = {hf.mad_crc(mad2[1:])};",
        "struct Type2Golden { Bytes memory; bool writable; Bytes prefix, suffix; };",
        "static const std::vector<Type2Golden> type2_goldens = {"]
record = ndef.encode([ndef.Record(4, b"example:data", b"opaque", b"id")])
memories = [
    b"\x03" + bytes([len(record)]) + record + b"\xfe",
    b"\x01\x03\xa0\x0c\x34\x03\0\xfe",
    b"\x01\x03\xa0\x0c\x24\x03\0\xfe",
    b"\x02\x03\xa4\x04\x04\x03\0\xfe",
    b"\x02\x03\x40\x04\x04\x03\0\xfe",
    b"\xfd\x03abc\x03" + bytes([len(record)]) + record + b"\xfd\x02xy\xfe",
]
for memory in memories:
    card = hf.CardInfo(b"\x04\0\0\0\0\0\0", family_hint="ntag213")
    snapshot = hf.Snapshot(card)
    for page in range(43):
        snapshot.blocks[page] = b"\0"*4
    snapshot.blocks[3] = b"\xe1\x10\x12\0"
    snapshot.blocks[41] = b"\0\0\0\xff"
    padded = memory.ljust(144, b"\0")
    for page in range(4, 40):
        snapshot.blocks[page] = padded[(page-4)*4:(page-3)*4]
    area = ntag.Type2NDEF(snapshot)
    def array(value):
        return "{" + ",".join(str(b) for b in value) + "}"
    out.append("{" + array(memory) + "," + ("true" if area.writable else "false") + ","
               + array(area.prefix) + "," + array(area.suffix) + "},")
out += ["};"]
pathlib.Path(__file__).with_name("hf_fixtures_golden.hpp").write_text("\n".join(out) + "\n")
assert len(examples) == len(ndef.FIELDS) == 19
assert hf.access_group(142) == 2 and hf.trailer_of(142) == 143
assert not hf.data_block(143, 256)
print("Generated all 19 desktop NDEF golden vectors.")
