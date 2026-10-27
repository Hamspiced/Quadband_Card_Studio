// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class ChameleonHFTests: XCTestCase {
    @MainActor func testCardIdentificationRejectsMalformedMultipleAndOtherCards() throws {
        let card = try ChameleonHFClient.decodeCard([4,1,2,3,4,4,0,8,0])
        XCTAssertTrue(card.isClassic1K); XCTAssertEqual(card.uid,[1,2,3,4]); XCTAssertEqual(card.atqa,[4,0]); XCTAssertTrue(card.atr.isEmpty)
        XCTAssertFalse(try ChameleonHFClient.decodeCard([4,1,2,3,4,4,0,0,0]).isClassic1K)
        for bad: [UInt8] in [[],[4,1,2,3,4],[4,1,2,3,4,4,0,8,5],[4,1,2,3,4,4,0,8,0,0]] { XCTAssertThrowsError(try ChameleonHFClient.decodeCard(bad)) }
    }
    @MainActor func testStandardKeysReadNDEFAndVerifiedRecordWritePreservesProtectedBlocks() async throws {
        let device = HFChameleonFixture(); let client = ChameleonClient(transport:device); _ = try await client.identify()
        let hf = ChameleonHFClient(client:client)
        let first = try await hf.read(key:Array(repeating:255,count:6),type:.a)
        XCTAssertEqual(first.blocks.count,64); XCTAssertTrue(first.errors.isEmpty)
        let original = try ClassicNDEF(snapshot:first); XCTAssertEqual(original.records.first?.uriValue,"https://midwestgadgets.org")
        let protected = device.memory.filter { !ClassicClient.isDataBlock($0.key) }
        let records = [try NDEFRecord.text("Chameleon HF test"),.uri("https://example.org")]
        let result = try await hf.writeNDEF(records,expected:original,key:Array(repeating:255,count:6),type:.a)
        XCTAssertEqual(try ClassicNDEF(snapshot:result).records,records)
        XCTAssertEqual(device.memory.filter { !ClassicClient.isDataBlock($0.key) },protected)
        XCTAssertEqual(device.writes.first?.1[1],0)
        XCTAssertTrue(device.writes.allSatisfy { ClassicClient.isDataBlock($0.0) })
    }
    @MainActor func testChangedCardStaleDataAndProtectedBlocksNeverWrite() async throws {
        let device = HFChameleonFixture(); let client = ChameleonClient(transport:device); _ = try await client.identify()
        let hf = ChameleonHFClient(client:client), key: [UInt8] = [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7]
        let expected = try await hf.identify()
        for block in [0,3,63] {
            do { _ = try await hf.write(block:block,data:Array(repeating:1,count:16),key:key,type:.a,expected:expected,previous:Array(repeating:0,count:16)); XCTFail() } catch {}
        }
        do { _ = try await hf.write(block:4,data:Array(repeating:1,count:16),key:key,type:.a,expected:expected,previous:Array(repeating:9,count:16)); XCTFail() } catch {}
        device.uid = [9,9,9,9]
        do { _ = try await hf.write(block:4,data:Array(repeating:1,count:16),key:key,type:.a,expected:expected,previous:device.memory[4]!); XCTFail() } catch {}
        XCTAssertTrue(device.writes.isEmpty)
    }
    @MainActor func testFailedWritesAreNotRetriedAndNDEFStaysUnpublished() async throws {
        let device = HFChameleonFixture(); let client = ChameleonClient(transport:device); _ = try await client.identify()
        let hf = ChameleonHFClient(client:client)
        let expected = try ClassicNDEF(snapshot:await hf.read(key:Array(repeating:255,count:6),type:.a))
        device.rejectBlock = 5
        do { _ = try await hf.writeNDEF([.text(String(repeating:"payload ",count:15))],expected:expected,key:Array(repeating:255,count:6),type:.a); XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("did not finish verification")) }
        XCTAssertEqual(device.writeAttempts.filter { $0 == 5 }.count,1)
        XCTAssertEqual(device.memory[4]?[1],0)
    }
    @MainActor func testDataOnlyProfileAndMissingWriteCapability() async throws {
        let device = HFChameleonFixture(); let client = ChameleonClient(transport:device); _ = try await client.identify()
        let hf = ChameleonHFClient(client:client)
        var source = try await hf.read(key:Array(repeating:255,count:6),type:.a)
        for block in 0..<64 where ClassicClient.isDataBlock(block) { source.blocks[block] = Array(repeating:UInt8(block),count:16) }
        let profile = try CardCredentials(snapshot:source), protected = device.memory.filter { !ClassicClient.isDataBlock($0.key) }
        _ = try await hf.writeProfile(profile,key:Array(repeating:255,count:6),type:.a)
        XCTAssertEqual(device.writes.count,47); XCTAssertEqual(device.memory.filter { !ClassicClient.isDataBlock($0.key) },protected)
        device.canWrite = false; _ = try await client.identify(); device.writes = []
        do { _ = try await hf.writeProfile(profile,key:Array(repeating:255,count:6),type:.a); XCTFail() } catch {}
        XCTAssertTrue(device.writes.isEmpty)
    }
}
@MainActor private final class HFChameleonFixture: ChameleonTransport {
    var uid: [UInt8] = [1,2,3,4]
    var memory = Dictionary(uniqueKeysWithValues:(0..<64).map { ($0,Array(repeating:UInt8(0),count:16)) })
    var writes: [(Int,[UInt8])] = [], writeAttempts: [Int] = []
    var rejectBlock: Int?, canWrite = true
    init() {
        var mad: [UInt8] = [0,1]+Array(repeating:[UInt8(3),0xE1],count:15).flatMap { $0 }; mad[0] = ClassicNDEF.crc(Array(mad.dropFirst()))
        memory[1] = Array(mad.prefix(16)); memory[2] = Array(mad.suffix(16))
        memory[3] = [0,0,0,0,0,0,0x78,0x77,0x88,0xC1,255,255,255,255,255,255]
        for sector in 1..<16 { memory[sector*4+3] = [0,0,0,0,0,0,0x7F,0x07,0x88,0x40,255,255,255,255,255,255] }
        let message = try! NDEFCodec.encode([.uri("https://midwestgadgets.org")])
        var area: [UInt8] = [3,UInt8(message.count)]+message+[0xFE]; area += Array(repeating:0,count:720-area.count)
        for (index,block) in (4..<64).filter({ $0%4 != 3 }).enumerated() { memory[block] = Array(area[index*16..<index*16+16]) }
    }
    func close() {}
    func exchange(_ command: UInt16,data:[UInt8],timeout:TimeInterval) async throws -> ChameleonFrame {
        func reply(_ status: UInt16 = 0x68, _ data: [UInt8] = []) -> ChameleonFrame { ChameleonFrame(command:command,status:status,data:data) }
        switch command {
        case 1000: return reply(0x68,[2,1])
        case 1033: return reply(0x68,[0])
        case 1002: return reply(0x68,[0])
        case 1001: return reply()
        case 1035:
            let codes: [UInt16] = [1001,1002,2000,2008]+(canWrite ? [2009] : [])
            return reply(0x68,codes.flatMap { [UInt8($0 >> 8),UInt8($0 & 255)] })
        case 2000: return reply(0,[UInt8(uid.count)]+uid+[4,0,8,0])
        case 2008,2009:
            guard data.count == (command == 2008 ? 8 : 24) else { XCTFail("Invalid native Classic command length"); return reply(0x60) }
            let block = Int(data[1]), key = Array(data[2..<8])
            let required: [UInt8] = block/4 == 0 ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7]
            guard key == required, data[0] == 0x60 else { return reply(6) }
            if command == 2008 { return reply(0,memory[block]!) }
            writeAttempts.append(block)
            if block == rejectBlock { return reply(6) }
            let bytes = Array(data.dropFirst(8)); memory[block] = bytes; writes.append((block,bytes)); return reply(0)
        default: XCTFail("Unexpected HF command \(command)"); return reply(0x67)
        }
    }
}
