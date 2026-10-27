// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class UHFTests: XCTestCase {
    let epc: [UInt8] = [0xE2,0x80,0x68,0x94,0,0,0x40,0x30,0xC6,0x12,0x8C,0xCC]
    func testManufacturerFramesAndEveryFragmentBoundary() throws {
        XCTAssertEqual(try UHFFrame(address:0,command:0x72).encoded(),[0xA0,3,0,0x72,0xEB])
        let captured = try UHFProtocol.hex("A0 19 00 89 01 30 00 E2 80 68 94 00 00 40 30 C6 12 8C CC E6 44 C7 F0 0D C4 6A 73")
        for split in 1..<captured.count {
            var decoder = UHFFrameDecoder()
            XCTAssertTrue(try decoder.accept(Array(captured.prefix(split))).isEmpty)
            let frame = try XCTUnwrap(decoder.accept(Array(captured.dropFirst(split))).first)
            let tag = try UHFProtocol.tag(frame.data)
            XCTAssertEqual(tag.epc,epc); XCTAssertEqual(tag.pc,[0x30,0]); XCTAssertEqual(tag.antenna,1); XCTAssertEqual(tag.frequencyKHz,902250)
            XCTAssertNotNil(tag.signal)
        }
        var decoder = UHFFrameDecoder()
        XCTAssertEqual(try decoder.accept(captured+captured).count,2)
    }
    func testMalformedFramesAndTagReportsFailClosed() throws {
        for bytes: [UInt8] in [[0,4,0,0,0,0],[0xA0,0],[0xA0,3,0,0x72,0]] {
            var decoder = UHFFrameDecoder(); XCTAssertThrowsError(try decoder.accept(bytes))
        }
        XCTAssertThrowsError(try UHFFrame(address:0,command:0x82,data:Array(repeating:0,count:253)).encoded())
        XCTAssertThrowsError(try UHFProtocol.tag([1,0xF8,0]))
        XCTAssertThrowsError(try UHFProtocol.version([2,3]))
        XCTAssertThrowsError(try UHFProtocol.version([2,3,99]))
    }
    func testCapturedLiveReadAndWrongTagRejection() throws {
        let payload = try UHFProtocol.hex("00 01 1C 30 00 E2 80 68 94 00 00 40 30 C6 12 8C CC 73 A6 E2 80 68 94 20 00 40 30 C6 12 8C CC 00 0C 01 01")
        let read = try UHFProtocol.parseRead(payload,expectedEPC:epc,count:6)
        XCTAssertEqual(read.bytes,try UHFProtocol.hex("E2 80 68 94 20 00 40 30 C6 12 8C CC"))
        XCTAssertThrowsError(try UHFProtocol.parseRead(payload,expectedEPC:[1,2],count:6))
        XCTAssertThrowsError(try UHFProtocol.parseRead(payload,expectedEPC:epc,count:5))
        for length in 0..<payload.count-1 { XCTAssertThrowsError(try UHFProtocol.parseRead(Array(payload.prefix(length)),expectedEPC:epc,count:6)) }
        var multiple = payload; multiple[1] = 2
        XCTAssertThrowsError(try UHFProtocol.parseRead(multiple,expectedEPC:epc,count:6))
    }
    func testEPCPayloadPreservesControlBitsAndValidatesWords() throws {
        XCTAssertEqual(try UHFProtocol.epcPayload([1,2,3,4],currentPC:[0x37,0xA5]),[0x17,0xA5,1,2,3,4])
        for bad in [[UInt8](),[1],[1,2,3],Array(repeating:UInt8(0),count:64)] { XCTAssertThrowsError(try UHFProtocol.epcPayload(bad,currentPC:[0x30,0])) }
        XCTAssertThrowsError(try UHFProtocol.writeCommand(bank:.reserved,word:0,bytes:[1,2],password:[0,0,0,0]))
        XCTAssertThrowsError(try UHFProtocol.writeCommand(bank:.epc,word:0,bytes:[1,2],password:[0,0,0,0]))
        XCTAssertThrowsError(try UHFProtocol.readCommand(bank:.user,word:0,count:65,password:[0,0,0,0]))
        XCTAssertEqual(try UHFProtocol.readCommand(bank:.tid,word:0x12345678,count:6,password:[1,2,3,4]),[2,0x12,0x34,0x56,0x78,0,6,1,2,3,4])
    }
    @MainActor func testEPCWriteUpdatesLengthAndVerifiesNewSelection() async throws {
        let fake = UHFFake(epc:epc); fake.pc = [0x35,0xA5]
        let new: [UInt8] = [1,2,3,4]
        let read = try await UHFClient(transport:fake).changeEPC(from:epc,to:new,password:[0,0,0,0])
        XCTAssertEqual(read.bytes,[0x15,0xA5]+new)
        let writes = fake.calls.filter { $0.0 == 0x82 }
        XCTAssertEqual(writes.count,1)
        XCTAssertEqual(writes[0].1,[0,0,0,0,1,0,0,0,1,0,3,0x15,0xA5]+new)
        XCTAssertEqual(fake.calls.filter { $0.0 == 0x85 }.last?.1,[0,4]+new)
    }
    @MainActor func testRejectedSelectionOrWrongTagPreventsWrite() async {
        for reject in [true,false] {
            let fake = UHFFake(epc:reject ? epc : [1,2]); fake.rejectSelect = reject
            do { _ = try await UHFClient(transport:fake).changeEPC(from:epc,to:[3,4],password:[0,0,0,0]); XCTFail() } catch {}
            XCTAssertFalse(fake.calls.contains { $0.0 == 0x82 })
        }
    }
    @MainActor func testWriteFailureOrVerificationMismatchNeverRetries() async {
        for fail in [true,false] {
            let fake = UHFFake(epc:epc); fake.failWrite = fail; fake.ignoreWrite = !fail
            do { _ = try await UHFClient(transport:fake).changeEPC(from:epc,to:[3,4],password:[0,0,0,0]); XCTFail() } catch { XCTAssertTrue(error.localizedDescription.contains(fail ? "not confirmed" : "verification failed")) }
            XCTAssertEqual(fake.calls.filter { $0.0 == 0x82 }.count,1)
        }
    }
    func testDetailedWriteErrorsAreNotAcceptedAsSuccess() throws {
        let packet: [UInt8] = [0,1,UInt8(epc.count+4),0x30,0]+epc+[0,0,0x33,1]
        XCTAssertThrowsError(try UHFProtocol.checkWrite(packet,oldEPC:epc,newEPC:nil))
        var okay = packet; okay[7+epc.count] = 0x10
        XCTAssertNoThrow(try UHFProtocol.checkWrite(okay,oldEPC:epc,newEPC:nil))
    }
}

@MainActor private final class UHFFake: UHFTransport {
    var onTag: ((UHFTag)->Void)?
    var epc: [UInt8], pc: [UInt8] = [0x30,0]
    var calls: [(UInt8,[UInt8])] = []
    var selection: [UInt8] = []
    var rejectSelect = false, failWrite = false, ignoreWrite = false
    init(epc: [UInt8]) { self.epc = epc }
    func exchange(_ command: UInt8,data: [UInt8],timeout: TimeInterval) async throws -> [UInt8] {
        calls.append((command,data))
        if command == 0x85 { selection = data; return [rejectSelect ? 0x41 : 0x10] }
        if command == 0x86 { return selection }
        if command == 0x81 {
            let count = Int(data[5]) << 8 | Int(data[6])
            let bytes = Array((pc+epc+Array(repeating:0,count:128)).prefix(count*2))
            return [0,1,UInt8(4+epc.count+bytes.count)] + pc + epc + [0,0] + bytes + [0,UInt8(bytes.count),1,1]
        }
        if command == 0x82 {
            if failWrite { return [0x33] }
            if !ignoreWrite { pc = Array(data[11...12]); epc = Array(data.dropFirst(13)) }
            return [0x10]
        }
        throw StripeError("Unexpected fake command")
    }
    func close() {}
}
