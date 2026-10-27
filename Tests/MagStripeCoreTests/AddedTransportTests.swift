// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class AddedTransportTests: XCTestCase {
    func testRawSwipeMappingsPreserveEmptyFieldsAndCRLF() throws {
        XCTAssertEqual(try KeyboardSwipeParser.parse("MIDWEST\r\n123=456\r\n789\r\n", format: .tracks123).tracks, ["MIDWEST", "123=456", "789"])
        XCTAssertEqual(try KeyboardSwipeParser.parse("\t123\t\r", format: .tracks123).tracks, ["", "123", ""])
        XCTAssertEqual(try KeyboardSwipeParser.parse("\u{2};123?\u{3}\r", format: .automatic).tracks, ["", "123", ""])
        XCTAssertEqual(try KeyboardSwipeParser.parse("  TEST  \r", format: .track1).tracks[0], "  TEST  ")
        XCTAssertThrowsError(try KeyboardSwipeParser.parse("123\t456\r", format: .track2))
        XCTAssertThrowsError(try KeyboardSwipeParser.parse("\r", format: .track2))
        XCTAssertEqual(KeyboardSwipeParser.visibleRaw("X\tY\r\n"), "X\\tY\\r\\n\n")
    }

    func testBluetoothResponsesAcrossEveryFragmentBoundary() throws {
        let read: [UInt8] = [0x1B,0x73,0x1B,1] + Array("%MIDWEST?".utf8) + [0x1B,2] + Array(";123?".utf8) + [0x3F,0x1C,0x1B,0x30]
        let cases: [([UInt8], [UInt8])] = [(MSRProtocol.ping,[0x1B,0x79]), ([0x1B,0x6F,7,5,5],[0x1B,0x30,7,5,5]), (MSRProtocol.read,read), (MSRProtocol.read,[0x1B,0x31])]
        for (command, reply) in cases {
            for boundary in 1..<reply.count {
                var stream = MSRStreamAssembler(command: command)
                XCTAssertNil(try stream.accept(Array(reply.prefix(boundary))))
                XCTAssertEqual(try stream.accept(Array(reply.dropFirst(boundary))), reply)
            }
        }
        var invalid = MSRStreamAssembler(command: MSRProtocol.ping)
        XCTAssertThrowsError(try invalid.accept([0x1B,0x79,0x30]))
        var overflow = MSRStreamAssembler(command: MSRProtocol.read)
        XCTAssertThrowsError(try overflow.accept([0x1B,0x73] + Array(repeating: 0, count: 4096)))
    }
}

final class NFCProtocolTests: XCTestCase {
    let key: [UInt8] = Array(repeating: 0xFF, count: 6)
    func testHexParsingAndBlockBoundaries() throws {
        XCTAssertEqual(try HexBytes.parse("00:01-02 03\n04 05", count: 6), [0,1,2,3,4,5])
        for text in ["0", "GG", "ＦＦ", "ZZ FF FF FF FF FF"] { XCTAssertThrowsError(try HexBytes.parse(text, count: 6)) }
        XCTAssertEqual((0..<64).filter(ClassicClient.isDataBlock).count, 47)
        for block in [-1,0,3,7,63,64,256] { XCTAssertFalse(ClassicClient.isDataBlock(block)) }
    }
    func testClassicReadUsesManufacturerCommandSequence() throws {
        let device = NFCFake(), client = ClassicClient(transport: NFCFake())
        XCTAssertTrue(try client.identify().isClassic1K)
        let result = try ClassicClient(transport: device).read(key: key, type: .a, sector: 1)
        XCTAssertEqual(result.blocks.count, 4)
        XCTAssertEqual(device.commands.prefix(4).map { $0 }, [
            [0xFF,0xCA,0,0,0], [0xFF,0x82,0,0,6] + key,
            [0xFF,0x86,0,0,5,1,0,4,0x60,0], [0xFF,0xB0,0,4,16]
        ])
    }
    func testPartialReadReportsKeyFailuresWithoutInventingBytes() throws {
        let device = NFCFake(); device.deniedSectors = [2,6]
        let result = try ClassicClient(transport: device).read(key: key, type: .b)
        XCTAssertEqual(result.blocks.count, 56)
        XCTAssertEqual(result.errors.count, 8)
        XCTAssertNil(result.blocks[8]); XCTAssertNotNil(result.errors[8])
        XCTAssertTrue(device.commands.contains([0xFF,0x86,0,0,5,1,0,4,0x61,0]))
    }
    func testWriteVerifiesAndKeepsOtherBlocksUnchanged() throws {
        let device = NFCFake(), data = Array("MIDWEST TEST 001".utf8)
        let client = ClassicClient(transport: device)
        let info = try client.identify(), before = device.memory
        XCTAssertEqual(try client.write(block: 4, data: data, key: key, type: .a, expectedCard: info, previous: before[4]!), data)
        XCTAssertEqual(device.commands.suffix(2).map { $0 }, [[0xFF,0xD6,0,4,16] + data,[0xFF,0xB0,0,4,16]])
        for block in 0..<64 where block != 4 { XCTAssertEqual(device.memory[block], before[block]) }
    }
    func testUnsafeOrStaleWritesNeverSendUpdateCommand() throws {
        for scenario in 0..<5 {
            let device = NFCFake(), client = ClassicClient(transport: NFCFake())
            let info = try client.identify()
            if scenario == 2 { device.uid = [9,9,9,9] }
            if scenario == 3 { device.atr[14] = 2 }
            let block = scenario == 0 ? 0 : scenario == 1 ? 7 : 4
            let previous = scenario == 4 ? Array(repeating: UInt8(0xEE), count: 16) : device.memory[block]!
            XCTAssertThrowsError(try ClassicClient(transport: device).write(block: block, data: Array(repeating: 0, count: 16), key: key, type: .a, expectedCard: info, previous: previous))
            XCTAssertFalse(device.commands.contains { $0[1] == 0xD6 })
        }
    }
    func testFailedWriteAndMismatchedVerificationAreNotSuccess() throws {
        for reject in [true,false] {
            let device = NFCFake(); device.rejectWrite = reject; device.ignoreWrite = !reject
            let client = ClassicClient(transport: device), info = try client.identify()
            XCTAssertThrowsError(try client.write(block: 4, data: Array(repeating: 0xAA, count: 16), key: key, type: .a, expectedCard: info, previous: device.memory[4]!)) { error in
                XCTAssertTrue(error.localizedDescription.contains("could not be verified"))
            }
        }
    }
    func testCredentialsRoundTripAndRejectMalformedProfiles() throws {
        let device = NFCFake()
        let snapshot = try ClassicClient(transport: device).read(key: key, type: .a)
        let profile = try CardCredentials(snapshot: snapshot)
        let data = try profile.encoded()
        let decoded = try CardCredentials.decode(data)
        XCTAssertEqual(decoded.blocks, profile.blocks)
        XCTAssertEqual(decoded.blocks.count, 47)
        XCTAssertFalse(String(decoding: data, as: UTF8.self).contains("uid"))
        XCTAssertFalse(String(decoding: data, as: UTF8.self).contains("key"))
        var invalid = profile
        invalid.blocks[0].number = 0
        XCTAssertThrowsError(try invalid.validatedBlocks())
        invalid = profile; invalid.blocks[0] = invalid.blocks[1]
        XCTAssertThrowsError(try invalid.validatedBlocks())
        invalid = profile; invalid.version = 999
        XCTAssertThrowsError(try invalid.validatedBlocks())
        invalid = profile; invalid.blocks[0].hex = "FF"
        XCTAssertThrowsError(try invalid.validatedBlocks())
        XCTAssertThrowsError(try CardCredentials.decode(Data(repeating: 0, count: 65537)))
        XCTAssertThrowsError(try CardCredentials(snapshot: ClassicSnapshot(card: snapshot.card, blocks: [1: Array(repeating: 0, count: 16)])))
    }
    func testProfileCopiesOnlyDataAndPreservesTargetUIDAndKeys() throws {
        let source = NFCFake(), target = NFCFake()
        let profile = try CardCredentials(snapshot: ClassicClient(transport: source).read(key: key, type: .a))
        target.uid = [9,8,7,6]
        target.memory = Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8(0xAA), count: 16)) })
        let original = target.memory
        let result = try ClassicClient(transport: target).writeProfile(profile, key: key, type: .a)
        XCTAssertEqual(result.uid, [9,8,7,6])
        for block in 0..<64 {
            XCTAssertEqual(target.memory[block], ClassicClient.isDataBlock(block) ? source.memory[block] : original[block])
        }
        XCTAssertEqual(target.commands.filter { $0[1] == 0xD6 }.count, 47)
        XCTAssertTrue(target.commands.filter { $0[1] == 0xD6 }.allSatisfy { ClassicClient.isDataBlock(Int($0[3])) })
    }
    func testBulkPreflightFailureDoesNotWriteAnyBlocks() throws {
        let source = NFCFake(), target = NFCFake()
        let profile = try CardCredentials(snapshot: ClassicClient(transport: source).read(key: key, type: .a))
        target.deniedSectors = [15]
        XCTAssertThrowsError(try ClassicClient(transport: target).writeProfile(profile, key: key, type: .a))
        XCTAssertFalse(target.commands.contains { $0[1] == 0xD6 })
    }
    func testBulkFailureReportsPartialCopyAndStopsThatCard() throws {
        let source = NFCFake(), target = NFCFake()
        let profile = try CardCredentials(snapshot: ClassicClient(transport: source).read(key: key, type: .a))
        target.memory = Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8(0xAA), count: 16)) })
        target.rejectWriteBlock = 8
        XCTAssertThrowsError(try ClassicClient(transport: target).writeProfile(profile, key: key, type: .a)) { error in
            XCTAssertTrue(error.localizedDescription.contains("verifying 5 of 47"))
        }
        XCTAssertFalse(target.commands.contains { $0[1] == 0xD6 && $0[3] > 8 })
    }
    func testTruncatedOrShortRepliesDoNotBecomeCardData() throws {
        let device = NFCFake(); device.shortRead = true
        XCTAssertThrowsError(try ClassicClient(transport: device).read(key: key, type: .a, sector: 0))
    }
}

private final class NFCFake: NFCTransport {
    var atr: [UInt8] = [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1,0,0,0,0,0x6A]
    var uid: [UInt8] = [1,2,3,4]
    var memory = Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8($0), count: 16)) })
    var commands: [[UInt8]] = []
    var deniedSectors: Set<Int> = []
    var rejectWriteBlock: Int?
    var rejectWrite = false, ignoreWrite = false, shortRead = false
    func transmit(_ bytes: [UInt8]) throws -> [UInt8] {
        commands.append(bytes)
        switch bytes[1] {
        case 0xCA: return uid + [0x90,0]
        case 0x82: return [0x90,0]
        case 0x86: return deniedSectors.contains(Int(bytes[7]) / 4) ? [0x63,0] : [0x90,0]
        case 0xB0: return (shortRead ? [0] : memory[Int(bytes[3])]!) + [0x90,0]
        case 0xD6:
            if rejectWrite || rejectWriteBlock == Int(bytes[3]) { return [0x63,0] }
            if !ignoreWrite { memory[Int(bytes[3])] = Array(bytes.dropFirst(5)) }
            return [0x90,0]
        default: XCTFail("Unexpected command"); return [0x6D,0]
        }
    }
}
