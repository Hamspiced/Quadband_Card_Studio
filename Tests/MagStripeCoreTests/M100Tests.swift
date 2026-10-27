// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class M100Tests: XCTestCase {
    let epc: [UInt8] = [0x30,0x75,0x1F,0xEB,0x70,0x5C,0x59,4,0xE3,0xD5,0x0D,0x70]
    let pc: [UInt8] = [0x34,0]
    func testVendorPacketsEveryFragmentAndCoalescing() throws {
        XCTAssertEqual(try M100Frame(command: 3, data: [0]).encoded(), [0xBB,0,3,0,1,0,4,0x7E])
        XCTAssertEqual(try M100Protocol.readCommand(bank: .user, word: 0, count: 2, password: [0,0,255,255]),
                       [0,0,255,255,3,0,0,0,2])
        let packet = try UHFProtocol.hex("BB02220011C9340030751FEB705C5904E3D50D703A76EF7E")
        for split in 1..<packet.count {
            var decoder = M100FrameDecoder()
            XCTAssertTrue(try decoder.accept(Array(packet.prefix(split))).isEmpty)
            let frame = try XCTUnwrap(decoder.accept(Array(packet.dropFirst(split))).first)
            let tag = try M100Protocol.tag(frame.data)
            XCTAssertEqual(tag.epc, epc); XCTAssertEqual(tag.pc, pc); XCTAssertEqual(tag.signal, -55)
            XCTAssertEqual(tag.frequencyKHz, 0)
        }
        var decoder = M100FrameDecoder()
        XCTAssertEqual(try decoder.accept(packet+packet).count, 2)
        var embedded = M100FrameDecoder()
        XCTAssertEqual(try embedded.accept(M100Frame(kind: 1, command: 0x39, data: [0x7E,0xBB,0x7E]).encoded()).first?.data, [0x7E,0xBB,0x7E])
    }
    func testMalformedFramesCRCAndBoundedLength() throws {
        let badPackets: [[UInt8]] = [[0xA0], [0xBB,0,0x22,0,0], [0xBB,1,0x22,4,1],
            [0xBB,1,0x28,0,1,0,0x2B,0x7E], [0xBB,1,0x28,0,1,0,0x2A,0], Array(repeating:0,count:65537)]
        for packet in badPackets { var decoder = M100FrameDecoder(); XCTAssertThrowsError(try decoder.accept(packet)) }
        XCTAssertThrowsError(try M100Frame(command: 3, data: Array(repeating: 0, count: 1025)).encoded())
        let report: [UInt8] = [0xC9]+pc+epc+[0x3A,0x76]
        XCTAssertThrowsError(try M100Protocol.tag(Array(report.dropLast())+[0]))
        for length in 0..<report.count { XCTAssertThrowsError(try M100Protocol.tag(Array(report.prefix(length)))) }
        XCTAssertThrowsError(try M100Protocol.version([0]+Array("Other reader".utf8)))
        XCTAssertEqual(try M100Protocol.version([0]+Array("M100 V1.00".utf8)), "M100 V1.00")
    }
    func testExactSelectionIncludesPCAndRejectsUnrepresentableLengths() throws {
        XCTAssertEqual(try M100Protocol.selection(epc: epc, pc: pc), [1,0,0,0,0x10,0x70,0]+pc+epc)
        XCTAssertThrowsError(try M100Protocol.selection(epc: epc, pc: [0x28,0]))
        XCTAssertThrowsError(try M100Protocol.selection(epc: Array(repeating:0,count:30), pc: [0x78,0]))
        XCTAssertEqual(try M100Protocol.selection(epc: Array(repeating:0,count:28), pc: [0x70,0]).count, 37)
    }
    func testAddressBankAndWriteLimitsFailBeforeTransport() throws {
        for (word, count): (UInt32, Int) in [(65536,1), (65535,2), (0,0), (0,65)] {
            XCTAssertThrowsError(try M100Protocol.readCommand(bank: .user, word: word, count: count, password: [0,0,0,0]))
        }
        for bank in [UHFMemoryBank.reserved, .tid] {
            XCTAssertThrowsError(try M100Protocol.writeCommand(bank: bank, word: 0, bytes: [1,2], password: [0,0,0,0]))
        }
        XCTAssertThrowsError(try M100Protocol.writeCommand(bank: .epc, word: 0, bytes: [1,2], password: [0,0,0,0]))
        XCTAssertThrowsError(try M100Protocol.writeCommand(bank: .user, word: 0, bytes: Array(repeating:0,count:66), password: [0,0,0,0]))
        XCTAssertThrowsError(try M100Protocol.memory([14]+pc+epc+[1], epc: epc, pc: pc, size: 2))
    }
    @MainActor func testEPCChangePreservesPCBitsAndVerifiesNewExactSelection() async throws {
        let fake = M100Fake(epc: epc, pc: pc), client: M100Client
        client = M100Client(transport: fake); client.observe(UHFTag(epc: epc, pc: pc))
        let result = try await client.changeEPC(from: epc, to: [1,2,3,4], password: [0,0,0,0])
        XCTAssertEqual(result.bytes, [0x14,0,1,2,3,4])
        XCTAssertEqual(fake.pc, [0x14,0])
        let writes = fake.calls.filter { $0.0 == 0x49 }
        XCTAssertEqual(writes.count, 1)
        XCTAssertEqual(writes.first?.1, [0,0,0,0,1,0,1,0,3,0x14,0,1,2,3,4])
        XCTAssertEqual(fake.filter, try M100Protocol.selection(epc: fake.epc, pc: fake.pc))
    }
    @MainActor func testUserWritePreservesIdentity() async throws {
        let fake = M100Fake(epc: epc, pc: pc), client: M100Client
        client = M100Client(transport: fake); client.observe(UHFTag(epc: epc, pc: pc))
        let result = try await client.writeUser(epc: epc, word: 2, bytes: [0xAA,0xBB], password: [0,0,0,0])
        XCTAssertEqual(result.bytes, [0xAA,0xBB]); XCTAssertEqual(fake.epc, epc); XCTAssertEqual(fake.pc, pc)
        XCTAssertEqual(fake.calls.filter { $0.0 == 0x49 }.count, 1)
    }
    @MainActor func testRejectedSelectionWrongTagAndUnseenTagsPreventWrites() async {
        for failure in 0..<3 {
            let fake = M100Fake(epc: epc, pc: pc), client: M100Client
            fake.rejectFilter = failure == 0; fake.wrongTag = failure == 1
            client = M100Client(transport: fake)
            if failure != 2 { client.observe(UHFTag(epc: epc, pc: pc)) }
            do { _ = try await client.writeUser(epc: epc, word: 0, bytes: [1,2], password: [0,0,0,0]); XCTFail() } catch {}
            XCTAssertFalse(fake.calls.contains { $0.0 == 0x49 })
        }
    }
    @MainActor func testUnverifiedWriteNeverRetries() async {
        for fail in [true, false] {
            let fake = M100Fake(epc: epc, pc: pc), client: M100Client
            fake.failWrite = fail; fake.ignoreWrite = !fail
            client = M100Client(transport: fake); client.observe(UHFTag(epc: epc, pc: pc))
            do { _ = try await client.writeUser(epc: epc, word: 0, bytes: [1,2], password: [0,0,0,0]); XCTFail() }
            catch { XCTAssertTrue(error.localizedDescription.contains("not verified")) }
            XCTAssertEqual(fake.calls.filter { $0.0 == 0x49 }.count, 1)
        }
    }
    @MainActor func testInventoryStopAndProfilePowerLimits() async throws {
        let fake = M100Fake(epc: epc, pc: pc)
        let reader = M100Client(transport: fake, profile: .yrm100)
        let version = try await reader.identify()
        XCTAssertTrue(version.contains("YRM100"))
        try await reader.startInventory()
        try await Task.sleep(nanoseconds: 20_000_000)
        try await reader.stopInventory()
        XCTAssertTrue(fake.calls.contains { $0.0 == 0x22 }); XCTAssertEqual(fake.calls.last?.0, 0x28)
        try await reader.setPower(26)
        let before = fake.calls.count
        do { try await reader.setBuzzer(true); XCTFail() } catch {}
        do { try await reader.setPower(27); XCTFail() } catch {}
        XCTAssertEqual(fake.calls.count, before)
        XCTAssertFalse(fake.calls.contains { [0x07,0xAB,0xAD].contains($0.0) })
        reader.close()
        XCTAssertTrue(fake.closed)
    }
}

@MainActor private final class M100Fake: UHFTransport {
    var onTag: ((UHFTag) -> Void)?
    var epc: [UInt8], pc: [UInt8], user = [UInt8](repeating: 0, count: 128)
    var filter: [UInt8] = [], calls: [(UInt8,[UInt8])] = []
    var rejectFilter = false, wrongTag = false, ignoreWrite = false, failWrite = false, closed = false
    var power: [UInt8] = [0x07,0xD0]
    init(epc: [UInt8], pc: [UInt8]) { self.epc = epc; self.pc = pc }
    func close() { closed = true }
    func exchange(_ command: UInt8, data: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        calls.append((command, data))
        switch command {
        case 3: return [0]+Array("M100 V1.00".utf8)
        case 0x0C: filter = data; return [0]
        case 0x0B: return rejectFilter ? [] : filter
        case 0x12, 0x28: return [0]
        case 0x22: try await Task.sleep(nanoseconds: 10_000_000); return []
        case 0xB6: power = data; return [0]
        case 0xB7: return power
        case 0x08: return [2]
        case 0x39:
            let word = Int(data[5]) << 8 | Int(data[6]), count = Int(data[7]) << 8 | Int(data[8])
            let memory = data[4] == 1 ? [0,0]+pc+epc+Array(repeating:0,count:128) : user
            var report = [UInt8(epc.count+2)]+pc+epc+Array(memory[word*2..<(word+count)*2])
            if wrongTag { report[3] ^= 1 }
            return report
        case 0x49:
            if failWrite { throw StripeError("Simulated failed tag write") }
            let report = [UInt8(epc.count+2)]+pc+epc+[0]
            if !ignoreWrite {
                if data[4] == 1 { pc = Array(data[9...10]); epc = Array(data.dropFirst(11)) }
                else {
                    let word = Int(data[5]) << 8 | Int(data[6])
                    user.replaceSubrange(word*2..<word*2+data.count-9, with: data.dropFirst(9))
                }
            }
            return report
        default: throw StripeError("Unexpected M100 fake command")
        }
    }
}
