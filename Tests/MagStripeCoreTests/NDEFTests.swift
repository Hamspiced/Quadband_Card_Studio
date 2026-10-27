// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class NDEFTests: XCTestCase {
    func testReferenceURLAndMessageBoundaries() throws {
        let bytes: [UInt8] = [0xD1,1,19,0x55,4] + Array("midwestgadgets.org".utf8)
        let record = NDEFRecord.uri("https://midwestgadgets.org")
        XCTAssertEqual(try NDEFCodec.encode([record]), bytes)
        XCTAssertEqual(try NDEFCodec.decode(bytes).first?.uriValue, "https://midwestgadgets.org")
        let long = NDEFRecord(tnf: 2, type: Array("application/octet-stream".utf8), identifier: [1,2,3], payload: Array(repeating: 0xAA, count: 300))
        let records = [record, try .text("Hello 🌎"), long]
        XCTAssertEqual(try NDEFCodec.decode(NDEFCodec.encode(records)), records)
        XCTAssertEqual(try NDEFCodec.decode([]), [])
        for truncated in 1..<bytes.count { XCTAssertThrowsError(try NDEFCodec.decode(Array(bytes.prefix(truncated)))) }
        let invalidMessages: [[UInt8]] = [[0x51] + bytes.dropFirst(), [0xB1] + bytes.dropFirst(), bytes + [0], [0xD7,0,0], [0xD0,1,0,0x55]]
        for invalid in invalidMessages { XCTAssertThrowsError(try NDEFCodec.decode(invalid)) }
        XCTAssertEqual(NDEFRecord(tnf: 1, type: [0x54], payload: [0x82,0x65,0x6E,0xFE,0xFF,0,0x48,0,0x69]).textValue, "Hi")
    }

    func testEveryRequestedRecordTypeProducesAValidRecord() throws {
        let values: [RecordKind: [String:String]] = [
            .text:["text":"  Test text  "], .url:["url":"https://midwestgadgets.org"], .customURI:["url":"myapp://open"],
            .unitLink:["url":"https://unit.link/midwest"], .search:["query":"NFC & cards"], .payment:["url":"https://example.org/pay"],
            .social:["url":"https://example.org/profile"], .video:["url":"https://example.org/video"], .file:["url":"https://example.org/file.pdf"],
            .application:["package":"com.example.app"], .mail:["email":"hello@example.org","subject":"Hello & NFC","body":"Two\nlines"],
            .contact:["name":"Doe; Jane","organization":"Midwest, Gadgets"], .phone:["phone":"+1 5551234567"],
            .location:["latitude":"42.3314","longitude":"-83.0458"], .sos:["name":"Test Name","medical":"Test info"],
            .bluetooth:["address":"00:11:22:33:44:55","name":"Test device"], .filament:[:],
            .wifi:["ssid":" Test Wi-Fi ","password":"12345678"], .data:["mime":"application/octet-stream","payload":"00 AA FF"]
        ]
        XCTAssertEqual(Set(values.keys), Set(RecordKind.allCases))
        for kind in RecordKind.allCases {
            let record = try RecordBuilder.make(kind, values: values[kind]!)
            XCTAssertEqual(try NDEFCodec.decode(NDEFCodec.encode([record])), [record], kind.rawValue)
        }
        XCTAssertEqual(try RecordBuilder.make(.text, values: values[.text]!).textValue, "  Test text  ")
        let contact = try RecordBuilder.make(.contact, values: values[.contact]!).displayValue
        XCTAssertTrue(contact.contains("FN:Doe\\; Jane\r\n")); XCTAssertTrue(contact.contains("ORG:Midwest\\, Gadgets"))
        let mail = URLComponents(string: try XCTUnwrap(RecordBuilder.make(.mail, values: values[.mail]!).uriValue))!
        XCTAssertEqual(mail.queryItems?.first?.value, "Hello & NFC")
        let bt = try RecordBuilder.make(.bluetooth, values: values[.bluetooth]!).payload
        XCTAssertEqual(Int(bt[0]) + Int(bt[1])*256, bt.count)
        XCTAssertEqual(Array(bt[2..<8]), [0x55,0x44,0x33,0x22,0x11,0])
        let wifi = try RecordBuilder.make(.wifi, values: values[.wifi]!)
        XCTAssertEqual(WSC.ssid(wifi.payload), " Test Wi-Fi ")
        let credential = try XCTUnwrap(WSC.fields(wifi.payload)?.first { $0.0 == 0x100E }?.1)
        let fields = try XCTUnwrap(WSC.fields(credential))
        XCTAssertEqual(fields.first { $0.0 == 0x1003 }?.1, [0,0x20])
        XCTAssertEqual(fields.first { $0.0 == 0x100F }?.1, [0,8])
        let json = try JSONSerialization.jsonObject(with: Data(RecordBuilder.make(.filament, values: [:]).payload)) as! [String:String]
        XCTAssertEqual(json["protocol"], "openspool"); XCTAssertEqual(json["version"], "1.0")
    }

    func testInvalidRecordFieldsAreRejected() {
        let cases: [(RecordKind,[String:String])] = [(.url,["url":"example.org"]),(.location,["latitude":"nan","longitude":"1"]),(.location,["latitude":"91","longitude":"0"]),(.bluetooth,["address":"GG"]),(.wifi,["ssid":"Test","password":"short"]),(.wifi,["ssid":String(repeating:"x",count:33),"security":"Open"]),(.data,["payload":"ABC"]),(.data,["mime":"not-a-mime"]),(.filament,["minTemp":"220","maxTemp":"190"]),(.application,["package":"bad name"])]
        for (kind, values) in cases { XCTAssertThrowsError(try RecordBuilder.make(kind, values: values), kind.rawValue) }
    }

    func testAutomaticReadRecoversAfterFailedAuthenticationAndReadsNDEF() throws {
        let device = NDEFDevice(), client = ClassicClient(transport: NDEFDevice())
        XCTAssertEqual(try client.read(key: NDEFDevice.factory, type: .a).blocks.count, 0)
        let snapshot = try ClassicClient(transport: device).readAutomatic(key: [1,2,3,4,5,6], type: .a)
        XCTAssertEqual(snapshot.blocks.count, 64); XCTAssertTrue(snapshot.errors.isEmpty)
        XCTAssertGreaterThanOrEqual(device.resets, 16)
        let ndef = try ClassicNDEF(snapshot: snapshot)
        XCTAssertEqual(ndef.records.first?.uriValue, "https://midwestgadgets.org")
        XCTAssertEqual(ndef.messageSize, 23); XCTAssertEqual(ndef.capacity, 716); XCTAssertTrue(ndef.writable)
        device.useFactory = true
        XCTAssertEqual(try ClassicClient(transport: device).readAutomatic(key: NDEFDevice.factory, type: .a).blocks.count, 64)
    }

    func testRecordWriteStagesLengthAndPreservesIdentityDirectoryAndKeys() throws {
        let device = NDEFDevice()
        let writer = ClassicClient(transport: device)
        let expected = try ClassicNDEF(snapshot: writer.readAutomatic(key: NDEFDevice.factory, type: .a))
        let before = device.memory
        let records = [try NDEFRecord.text(String(repeating: "Long payload ", count: 30)), .uri("https://example.org")]
        let result = try writer.writeNDEF(records, expected: expected, key: NDEFDevice.factory, type: .a)
        XCTAssertEqual(try ClassicNDEF(snapshot: result).records, records)
        XCTAssertEqual(device.writes.first?.0, 4); XCTAssertEqual(device.writes.first?.1[1], 0)
        XCTAssertEqual(device.writes.last?.0, 4); XCTAssertEqual(device.writes.last?.1[1], 255)
        for block in [0,1,2] + Array(stride(from: 3, to: 64, by: 4)) { XCTAssertEqual(device.memory[block], before[block]) }
        XCTAssertTrue(device.writes.allSatisfy { $0.0 >= 4 && $0.0 % 4 != 3 })
    }

    func testUnsafeRecordWritesNeverUpdateMemory() throws {
        for scenario in 0..<5 {
            let device = NDEFDevice(), writer: ClassicClient
            writer = ClassicClient(transport: device)
            if scenario == 0 { for sector in 1..<16 { device.memory[sector*4+3]![9] = 0x43 } }
            let expected = try ClassicNDEF(snapshot: writer.readAutomatic(key: NDEFDevice.factory, type: .a))
            if scenario == 1 { device.uid = [9,9,9,9] }
            if scenario == 2 { device.memory[5]![0] ^= 1 }
            if scenario == 3 { device.memory[3]![9] = 0 }
            let records = [try NDEFRecord.text(String(repeating: "x", count: scenario == 4 ? 800 : 10))]
            XCTAssertThrowsError(try writer.writeNDEF(records, expected: expected, key: NDEFDevice.factory, type: .a))
            XCTAssertTrue(device.writes.isEmpty, "scenario \(scenario)")
        }
    }

    func testPartialRecordWriteIsReportedAndDoesNotCommitLength() throws {
        let device = NDEFDevice()
        let client = ClassicClient(transport: device)
        let expected = try ClassicNDEF(snapshot: client.readAutomatic(key: NDEFDevice.factory, type: .a))
        device.rejectBlock = 5
        XCTAssertThrowsError(try client.writeNDEF([.text(String(repeating:"changed",count:30))], expected: expected, key: NDEFDevice.factory, type: .a)) { error in
            XCTAssertTrue(error.localizedDescription.contains("did not finish verification"))
        }
        XCTAssertEqual(device.memory[4]![1], 0)
    }

    func testOtherTLVsIncludingTrailingZeroBytesArePreserved() throws {
        let device = NDEFDevice()
        let message = try NDEFCodec.encode([NDEFRecord.uri("https://midwestgadgets.org")])
        let original: [UInt8] = [0xFD,2,0xAB,0] + [3,UInt8(message.count)] + message + [0xFD,3,0x12,0,0,0xFE]
        device.setArea(original)
        let ndef = try ClassicNDEF(snapshot: ClassicClient(transport: device).readAutomatic(key: NDEFDevice.factory, type: .a))
        let replacement = try ndef.replacing(with: [.text("Hi")])
        XCTAssertEqual(Array(replacement.prefix(4)), [0xFD,2,0xAB,0])
        device.setArea(replacement)
        let reread = try ClassicNDEF(snapshot: ClassicClient(transport: device).readAutomatic(key: NDEFDevice.factory, type: .a))
        XCTAssertEqual(reread.suffix, [0xFD,3,0x12,0,0])
        XCTAssertEqual(reread.records.first?.textValue, "Hi")
    }
}

private final class NDEFDevice: NFCTransport {
    static let factory = Array(repeating: UInt8(255), count: 6)
    var atr: [UInt8] = [0x3B,0x8F,0x80,1,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1,0,0,0,0,0x6A]
    var uid: [UInt8] = [1,2,3,4]
    var memory = Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8(0), count: 16)) })
    var writes: [(Int,[UInt8])] = []
    var loaded: [UInt8] = [], authenticated: Int?, halted = false, resets = 0, useFactory = false
    var rejectBlock: Int?
    init() {
        var mad: [UInt8] = [0,1] + Array(repeating: [UInt8(3),0xE1], count: 15).flatMap { $0 }
        mad[0] = ClassicNDEF.crc(Array(mad.dropFirst()))
        memory[1] = Array(mad.prefix(16)); memory[2] = Array(mad.suffix(16))
        memory[3] = [0,0,0,0,0,0,0x78,0x77,0x88,0xC1,255,255,255,255,255,255]
        for sector in 1..<16 { memory[sector*4+3] = [0,0,0,0,0,0,0x7F,0x07,0x88,0x40,255,255,255,255,255,255] }
        let message = try! NDEFCodec.encode([.uri("https://midwestgadgets.org")])
        setArea([3,UInt8(message.count)] + message + [0xFE])
    }
    func setArea(_ bytes: [UInt8]) {
        let padded = bytes + Array(repeating: UInt8(0), count: max(0,720-bytes.count))
        for (index, block) in (4..<64).filter({ $0 % 4 != 3 }).enumerated() { memory[block] = Array(padded[index*16..<index*16+16]) }
    }
    func resetAuthentication() { halted = false; authenticated = nil; loaded = []; resets += 1 }
    func transmit(_ bytes: [UInt8]) throws -> [UInt8] {
        switch bytes[1] {
        case 0xCA: return uid + [0x90,0]
        case 0x82: loaded = Array(bytes.dropFirst(5)); return [0x90,0]
        case 0x86:
            let sector = Int(bytes[7])/4
            let key: [UInt8] = useFactory ? Self.factory : sector == 0 ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7]
            guard !halted, loaded == key else { halted = true; authenticated = nil; return [0x63,0] }
            authenticated = sector; return [0x90,0]
        case 0xB0:
            guard authenticated == Int(bytes[3])/4 else { return [0x63,0] }
            return memory[Int(bytes[3])]! + [0x90,0]
        case 0xD6:
            let block = Int(bytes[3])
            guard authenticated == block/4, block != rejectBlock else { return [0x63,0] }
            let value = Array(bytes.dropFirst(5)); writes.append((block,value)); memory[block] = value; return [0x90,0]
        default: XCTFail("Unexpected NFC command"); return [0x6D,0]
        }
    }
}
