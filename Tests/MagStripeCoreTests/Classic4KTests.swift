import XCTest
@testable import MagStripeCore

final class Classic4KTests: XCTestCase {
    func testDirectPCSCFullReadLastSectorAndVerifiedDataProfile() throws {
        let device = FourKPCSCFixture(), client = ClassicClient(transport:device), key = Array(repeating:UInt8(255),count:6)
        let snapshot = try client.read(key:key,type:.a)
        XCTAssertEqual(snapshot.card.family,.classic4k); XCTAssertEqual(snapshot.blocks.count,256)
        let last = try client.read(key:key,type:.a,sector:39)
        XCTAssertEqual(Set(last.blocks.keys),Set(240...255))
        var source = snapshot
        for block in CardFamily.classic4k.dataUnits { source.blocks[block] = Array(repeating:UInt8(block),count:16) }
        let before = device.memory.filter { !CardFamily.classic4k.isData($0.key) }
        _ = try client.writeProfile(CardCredentials(snapshot:source),key:key,type:.a)
        XCTAssertEqual(device.writes.count,215)
        XCTAssertEqual(device.memory.filter { !CardFamily.classic4k.isData($0.key) },before)
        let changed = Array(repeating:UInt8(0xA5),count:16)
        XCTAssertEqual(try client.write(block:254,data:changed,key:key,type:.a,expectedCard:snapshot.card,previous:device.memory[254]!),changed)
        for block in [0,3,127,143,255,256] {
            XCTAssertThrowsError(try client.write(block:block,data:changed,key:key,type:.a,expectedCard:snapshot.card,previous:changed))
        }
        XCTAssertEqual(device.writes.count,216)
    }
    func testDirectPCSCAutomaticMAD2KeyAndNDEFPreservation() throws {
        let device = FourKPCSCFixture(); try device.loadGoldenNDEF(); device.enforceStandardKeys = true
        let client = ClassicClient(transport:device), key = Array(repeating:UInt8(255),count:6)
        let snapshot = try client.readAutomatic(key:key,type:.a), expected = try ClassicNDEF(snapshot:snapshot)
        XCTAssertEqual(snapshot.sectorKeys[16]?.bytes,[0xA0,0xA1,0xA2,0xA3,0xA4,0xA5])
        let protected = device.memory.filter { !expected.blocks.contains($0.key) }
        let result = try client.writeNDEF([.text("Direct 4K")],expected:expected,key:key,type:.a)
        XCTAssertEqual(try ClassicNDEF(snapshot:result).records.first?.textValue,"Direct 4K")
        XCTAssertEqual(device.memory.filter { !expected.blocks.contains($0.key) },protected)
    }
    @MainActor func testNativeChameleon4KReadProfileAndMAD2NDEF() async throws {
        let device = FourKChameleonFixture(), client = ChameleonClient(transport:device)
        _ = try await client.identify()
        let hf = ChameleonHFClient(client:client), key = Array(repeating:UInt8(255),count:6)
        var snapshot = try await hf.read(key:key,type:.a)
        XCTAssertEqual(snapshot.blocks.count,256); XCTAssertEqual(snapshot.card.family,.classic4k)
        for block in CardFamily.classic4k.dataUnits { snapshot.blocks[block] = Array(repeating:UInt8(block),count:16) }
        _ = try await hf.writeProfile(CardCredentials(snapshot:snapshot),key:key,type:.a)
        XCTAssertEqual(device.card.writes.count,215)
        try device.card.loadGoldenNDEF(); device.card.enforceStandardKeys = true
        let expected = try ClassicNDEF(snapshot:await hf.read(key:key,type:.a))
        let protected = device.card.memory.filter { !expected.blocks.contains($0.key) }
        let result = try await hf.writeNDEF([.text("Chameleon 4K")],expected:expected,key:key,type:.a)
        XCTAssertEqual(try ClassicNDEF(snapshot:result).records.first?.textValue,"Chameleon 4K")
        XCTAssertEqual(device.card.memory.filter { !expected.blocks.contains($0.key) },protected)
    }
}

private final class FourKPCSCFixture: NFCTransport {
    let atr: [UInt8] = [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,2,0,0,0,0,0]
    var memory: [Int:[UInt8]]
    var writes: [Int] = []
    var enforceStandardKeys = false
    var key: [UInt8] = []
    init() {
        memory = Dictionary(uniqueKeysWithValues:(0..<256).map { ($0,Array(repeating:UInt8(0),count:16)) })
        for sector in 0..<40 { memory[CardFamily.classic4k.blocks(in:sector).last!] = Array(repeating:255,count:6)+[0xFF,0x07,0x80,0x69]+Array(repeating:255,count:6) }
    }
    func accepts(_ block: Int, key: [UInt8]) -> Bool {
        let sector = CardFamily.classic4k.sector(of:block)
        let required: [UInt8] = sector == 0 || sector == 16 ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7]
        return !enforceStandardKeys || key == required
    }
    func transmit(_ bytes: [UInt8]) throws -> [UInt8] {
        switch bytes[1] {
        case 0xCA: return [1,2,3,4,0x90,0]
        case 0x82: key = Array(bytes.dropFirst(5)); return [0x90,0]
        case 0x86: return accepts(Int(bytes[7]),key:key) ? [0x90,0] : [0x63,0]
        case 0xB0: return memory[Int(bytes[3])]!+[0x90,0]
        case 0xD6: memory[Int(bytes[3])] = Array(bytes.dropFirst(5)); writes.append(Int(bytes[3])); return [0x90,0]
        default: throw StripeError("Unexpected simulated PC/SC command.")
        }
    }
    func loadGoldenNDEF() throws {
        let object = try XCTUnwrap(BridgeFixtures.object()["classic4kNdef"] as? [String:Any])
        for entry in object["blocks"] as! [[String:Any]] { memory[entry["number"] as! Int] = try BridgeFixtures.bytes(entry["hex"] as! String) }
    }
}

@MainActor private final class FourKChameleonFixture: ChameleonTransport {
    let card = FourKPCSCFixture()
    func close() {}
    func exchange(_ command: UInt16,data:[UInt8],timeout:TimeInterval) async throws -> ChameleonFrame {
        func reply(_ data: [UInt8] = [], _ status: UInt16 = 0x68) -> ChameleonFrame { ChameleonFrame(command:command,status:status,data:data) }
        switch command {
        case 1000: return reply([2,1])
        case 1033,1002: return reply([0])
        case 1001: return reply()
        case 1035: return reply([UInt16(1001),1002,2000,2008,2009].flatMap { [UInt8($0 >> 8),UInt8(truncatingIfNeeded:$0)] })
        case 2000: return reply([4,1,2,3,4,2,0,0x18,0],0)
        case 2008,2009:
            let block = Int(data[1]), key = Array(data[2..<8])
            guard card.accepts(block,key:key) else { return reply([],6) }
            if command == 2008 { return reply(card.memory[block]!,0) }
            card.memory[block] = Array(data.dropFirst(8)); card.writes.append(block); return reply([],0)
        default: throw StripeError("Unexpected simulated Chameleon command.")
        }
    }
}
