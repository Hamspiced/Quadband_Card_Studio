// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class ChameleonTests: XCTestCase {
    func testGoldenFramesFragmentationAndChecksums() throws {
        XCTAssertEqual(try ChameleonFrame(command:1000).encoded(),[0x11,0xEF,3,0xE8,0,0,0,0,0x15,0])
        let raw: [UInt8] = [0x11,0xEF,3,0xE8,0,0x68,0,2,0xAB,2,1,0xFD]
        var decoder = ChameleonDecoder()
        XCTAssertEqual(try decoder.accept(Array(raw.prefix(4))),[])
        XCTAssertEqual(try decoder.accept(Array(raw.dropFirst(4))+raw),[ChameleonFrame(command:1000,status:0x68,data:[2,1]),ChameleonFrame(command:1000,status:0x68,data:[2,1])])
        for index in [0,1,4,8,9,11] {
            var bad = raw; bad[index] ^= 1; var parser = ChameleonDecoder(); XCTAssertThrowsError(try parser.accept(bad))
        }
        var tooLong: [UInt8] = [17,239,3,232,0,104,0x20,0]; tooLong.append(0 &- tooLong.reduce(0,&+))
        var parser = ChameleonDecoder(); XCTAssertThrowsError(try parser.accept(tooLong))
    }
    func testOlderAndNewerEMResponsesAndFileValidation() throws {
        let id: [UInt8] = [1,2,3,4,5]
        let card = try LFCard.decode(id,format:.em410x)
        XCTAssertEqual(card,try LFCard.decode([0,100]+id,format:.em410x))
        XCTAssertEqual(card,try LFCard.fromFile(card.fileData()))
        XCTAssertThrowsError(try LFCard.decode([0,99]+id,format:.em410x))
        XCTAssertThrowsError(try LFCard.decode([1,2],format:.em410x))
        let file = Data(#"{"schema":1,"kind":"Quadband LF card","card":{"format":"EM410x","bytes":[1]}}"#.utf8)
        XCTAssertThrowsError(try LFCard.fromFile(file))
        XCTAssertThrowsError(try LFCard.fromFile(Data(repeating:0,count:16385)))
        let electra = try LFCard.decode([0,104]+Array(repeating:1,count:13),format:.em410x)
        XCTAssertEqual(electra.format,.electra)
    }
    func testHIDAndIOProxPadding() throws {
        let hid: [UInt8] = [1,0,0,0,10,0,0,0,4,210,0,0,0]
        XCTAssertEqual(try LFCard.decode(hid+[0,0,0],format:.hid).bytes,hid)
        XCTAssertThrowsError(try LFCard(format:.hid,bytes:[0]+Array(hid.dropFirst())))
        let io = try LFCard(format:.ioprox,bytes:Array(repeating:0,count:16))
        XCTAssertTrue(io.matches(try LFCard(format:.ioprox,bytes:Array(repeating:0,count:12)+[1,2,3,4])))
        XCTAssertFalse(io.matches(try LFCard(format:.ioprox,bytes:[1]+Array(repeating:0,count:15))))
    }
    @MainActor func testIdentificationIsReadOnlyAndCapabilitiesGateWrites() async throws {
        let fake = FakeChameleon(); fake.caps = [1001,1002,3000]
        let client = ChameleonClient(transport:fake); let info = try await client.identify()
        XCTAssertEqual(info.formats,[.em410x]); XCTAssertFalse(fake.calls.contains { $0.0 == 1001 })
        let card = try LFCard(format:.em410x,bytes:[1,2,3,4,5])
        do { _ = try await client.writeAndVerify(card); XCTFail() } catch {}
        XCTAssertFalse(fake.calls.contains { $0.0 == 3001 || $0.0 == 1001 })
        fake.version = [3,0]
        do { _ = try await ChameleonClient(transport:fake).identify(); XCTFail() } catch {}
        fake.version = [2,1]; fake.model = [1]
        do { _ = try await ChameleonClient(transport:fake).identify(); XCTFail() } catch {}
    }
    @MainActor func testPasswordFreeBlocksAndGuard() throws {
        let card = try LFCard(format:.em410x,bytes:[1,2,3,4,5])
        let words = try T5577.blocks(card)
        XCTAssertEqual(words.map { $0.0 },[1,2,0])
        XCTAssertEqual(words.last?.1,0x00148040)
        for (block,word) in words {
            let payload = try T5577.payload(block:block,word:word)
            XCTAssertEqual(payload.count,11); XCTAssertEqual(Array(payload.suffix(6)),Array(repeating:0,count:6))
        }
        XCTAssertThrowsError(try T5577.payload(block:7,word:0))
        XCTAssertThrowsError(try T5577.payload(block:0,word:0x00148050))
        for command in T5577.passwordWriters { XCTAssertThrowsError(try T5577.validateCommand(command,data:[])) }
        XCTAssertThrowsError(try T5577.blocks(LFCard(format:.hid,bytes:[1]+Array(repeating:0,count:12))))
    }
    @MainActor func testLegacyPasswordCommandsCannotReachTransport() async throws {
        let fake = FakeChameleon(), client: ChameleonClient
        client = ChameleonClient(transport:fake)
        for command in T5577.passwordWriters {
            do { _ = try await client.command(command); XCTFail() } catch {}
        }
        XCTAssertTrue(fake.calls.isEmpty)
    }
    @MainActor func testVerifiedWriteAndFailedVerificationNeverRetryWrite() async throws {
        let fake = FakeChameleon(); let client = ChameleonClient(transport:fake); _ = try await client.identify()
        let card = try LFCard(format:.em410x,bytes:[1,2,3,4,5]); fake.scanData = card.bytes
        let result = try await client.writeAndVerify(card)
        XCTAssertEqual(result,card); XCTAssertEqual(fake.calls.filter { $0.0 == 3016 }.count,3)
        fake.scanData = [9,9,9,9,9]
        do { _ = try await client.writeAndVerify(card); XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("Write not verified")) }
        XCTAssertEqual(fake.calls.filter { $0.0 == 3016 }.count,6)
        fake.writeThrows = true
        do { _ = try await client.writeAndVerify(card); XCTFail() } catch {}
        XCTAssertEqual(fake.calls.filter { $0.0 == 3016 }.count,7)
    }
    @MainActor func testAutomaticScanTriesAvailableFormatsAndRestoresMode() async throws {
        let fake = FakeChameleon(); fake.caps = [1001,1002,3000,3001,3004,3005]; fake.noEM = true
        let client = ChameleonClient(transport:fake); _ = try await client.identify()
        let card = try await client.scan(); XCTAssertEqual(card?.format,.viking)
        XCTAssertFalse(fake.calls.contains { $0.0 == 3002 })
        try await client.restoreMode(); XCTAssertEqual(fake.calls.last?.0,1001); XCTAssertEqual(fake.calls.last?.1,[0])
    }
}
@MainActor private final class FakeChameleon: ChameleonTransport {
    var calls: [(UInt16,[UInt8])] = []
    var version: [UInt8] = [2,1], model: [UInt8] = [0], scanData: [UInt8] = [1,2,3,4,5]
    var caps: [UInt16] = [1001,1002,3000,3016]
    var writeThrows = false, noEM = false
    func close() {}
    func exchange(_ command: UInt16,data:[UInt8],timeout:TimeInterval) async throws -> ChameleonFrame {
        calls.append((command,data))
        switch command {
        case 1000: return ChameleonFrame(command:command,status:0x68,data:version)
        case 1033: return ChameleonFrame(command:command,status:0x68,data:model)
        case 1035: return ChameleonFrame(command:command,status:0x68,data:caps.flatMap { [UInt8($0 >> 8),UInt8($0 & 255)] })
        case 1002: return ChameleonFrame(command:command,status:0x68,data:[0])
        case 1001: return ChameleonFrame(command:command,status:0x68)
        case 3000: return ChameleonFrame(command:command,status:noEM ? 0x41 : 0x40,data:noEM ? [] : scanData)
        case 3004: return ChameleonFrame(command:command,status:0x40,data:[1,2,3,4])
        case 3016:
            if writeThrows { throw StripeError("Disconnected") }; return ChameleonFrame(command:command,status:0x40)
        default: throw StripeError("Unexpected command in test: \(command)")
        }
    }
}
