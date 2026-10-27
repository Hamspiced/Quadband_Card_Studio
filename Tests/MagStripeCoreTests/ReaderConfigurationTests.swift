// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class ReaderConfigurationTests: XCTestCase {
    func snapshot() -> ReaderSettingsSnapshot {
        var s = ReaderSettingsSnapshot(reader:"ACS ACR1552 1S CL Reader",family:.acr1552,firmware:"ACR1552 R FW 1.06.00",serial:[1,2,3])
        s.values = [.polling:[0x8B],.types:[7,5],.pps:[3,0],.ui:[0xCF],.hid:[0xC0,0],.characters:[255,0x28,255],.keyboard:[0],.host:[1],.power:[0],.suspend:[0]]
        return s
    }
    func testReaderFamilyDetectionExcludesSAMAndWrongReader() {
        XCTAssertEqual(ReaderFamily.detect("ACS ACR1552 1S CL Reader 0"),.acr1552)
        XCTAssertEqual(ReaderFamily.detect("ACS ACR122U PICC Interface"),.acr122)
        for name in ["ACS ACR1552 SAM", "ACS ACR1552 Contact Reader", "ACR15520", "ACR1222U", "ELYCTIS CL Reader"] { XCTAssertNil(ReaderFamily.detect(name)) }
    }
    func testReadModifyWritePreservesOtherUIBits() throws {
        let s = snapshot(), changes = try ReaderConfigPlan.make(snapshot:s,edits:ReaderSettingsCatalog.preset("mute",family:.acr1552,snapshot:s))
        XCTAssertEqual(changes.count,1); XCTAssertEqual(changes[0].register,.ui); XCTAssertEqual(changes[0].after,[0xC7])
        XCTAssertEqual(try ReaderConfigPlan.make(snapshot:s,edits:["uppercase":16])[0].after,[0xD0,0])
        XCTAssertEqual(try ReaderConfigPlan.make(snapshot:s,edits:["pulse":0])[0].after,[0x89])
        XCTAssertEqual(try ReaderConfigPlan.make(snapshot:s,edits:["pulse":1000])[0].after,[0xAB])
    }
    func testFirmwareGatesUnsupportedAndUnknownSettings() throws {
        var s = snapshot(); s = ReaderSettingsSnapshot(reader:s.reader,family:.acr1552,firmware:"ACR1552 R FW 1.03.02")
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:["power":5]))
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:["madeUp":1]))
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:["presenceBeep":0]))
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:["power":255]))
    }
    func testUnsafeUSBCombinationAndEmptyTechnologiesRejected() throws {
        var s = snapshot(); s.values[.suspend] = [1]
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:["host":2]))
        let plan = try ReaderConfigPlan.make(snapshot:s,edits:["host":2,"suspend":0])
        XCTAssertEqual(plan.map(\.register),[.suspend,.host])
        let fields = ReaderSettingsCatalog.fields(.acr1552).filter { $0.register == .types }
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:Dictionary(uniqueKeysWithValues:fields.map { ($0.id,0) })))
    }
    func testACR122CommandsAreModelSpecific() throws {
        var s = ReaderSettingsSnapshot(reader:"ACR122U",family:.acr122,firmware:"ACR122U201"); s.values[.operating] = [255]
        XCTAssertEqual(ReaderRegister.buzzer.setCommand(.acr122,[0]),[255,0,0x52,0,0])
        XCTAssertEqual(ReaderRegister.rf.setCommand(.acr122,[0]),[255,0,0,0,4,0xD4,0x32,1,0])
        XCTAssertThrowsError(try ReaderConfigPlan.make(snapshot:s,edits:["power":5]))
        XCTAssertEqual(try ReaderConfigPlan.make(snapshot:s,edits:["interval":0])[0].after,[0xDF])
    }
    func testResponseValidation() throws {
        XCTAssertEqual(try ReaderConfigurationClient.escapePayload([0xE1,0,0,0,2,7,5]),[7,5])
        XCTAssertEqual(try ReaderConfigurationClient.operatingByte([0x90,0xFF]),[0xFF])
        for bad: [UInt8] in [[],[0xE1,0,0,0,2,7],[0xE1,0,1,0,1,7],[0x90,0]] { XCTAssertThrowsError(try ReaderConfigurationClient.escapePayload(bad)) }
    }
    func testSuccessfulApplyChecksIdentityAndReadsBack() async throws {
        let s = snapshot(), fake = ReaderConfigFake(s)
        let results = try await ReaderConfigurationClient(transport:fake).apply(snapshot:s,edits:["presenceBeep":0])
        XCTAssertEqual(results.count,1); XCTAssertTrue(results[0].success); XCTAssertTrue(results[0].verified)
        let commands = await fake.commands
        XCTAssertEqual(commands.filter { $0.count > 5 },[[0xE0,0,0,0x21,1,0xC7]])
    }
    func testStaleOrSwappedReaderNeverWrites() async throws {
        for swapped in [true,false] {
            let s = snapshot(); var current = s
            if swapped { current.serial = [9] } else { current.values[.ui] = [0xFF] }
            let fake = ReaderConfigFake(current)
            do { _ = try await ReaderConfigurationClient(transport:fake).apply(snapshot:s,edits:["presenceBeep":0]); XCTFail() } catch {}
            let commands = await fake.commands; XCTAssertFalse(commands.contains { $0.count > 5 })
        }
    }
    func testPartialFailureStopsFurtherSettings() async throws {
        let s = snapshot(), fake = ReaderConfigFake(s,failAddress:0x21)
        let results = try await ReaderConfigurationClient(transport:fake).apply(snapshot:s,edits:["pulse":500,"presenceBeep":0,"power":5])
        XCTAssertEqual(results.count,2); XCTAssertTrue(results[0].success); XCTAssertFalse(results[1].success)
        let commands = await fake.commands; XCTAssertFalse(commands.contains { $0.count > 5 && $0[3] == 0x50 })
    }
    func testPowerDisconnectIsVerifiedWithoutResending() async throws {
        let s = snapshot(), fake = ReaderConfigFake(s,disconnectPower:true)
        let results = try await ReaderConfigurationClient(transport:fake).apply(snapshot:s,edits:["power":4])
        XCTAssertTrue(results[0].success)
        let commands = await fake.commands; XCTAssertEqual(commands.filter { $0.count > 5 && $0[3] == 0x50 }.count,1)
    }
}
private actor ReaderConfigFake: ReaderControlTransport {
    var snapshot: ReaderSettingsSnapshot
    var commands: [[UInt8]] = []
    let failAddress: UInt8?
    let disconnectPower: Bool
    init(_ snapshot: ReaderSettingsSnapshot,failAddress: UInt8? = nil,disconnectPower: Bool = false) { self.snapshot = snapshot; self.failAddress = failAddress; self.disconnectPower = disconnectPower }
    func control(_ command: [UInt8]) throws -> [UInt8] {
        commands.append(command)
        func frame(_ bytes: [UInt8]) -> [UInt8] { [0xE1,0,0,0,UInt8(bytes.count)]+bytes }
        if command[3] == 0x18 { return frame(Array(snapshot.firmware.utf8)) }
        if command[3] == 0x33 { return frame(snapshot.serial ?? []) }
        guard let reg = ReaderRegister.allCases.first(where: { $0.address == command[3] }) else { throw StripeError("Unknown register") }
        if command.count > 5 {
            if command[3] == failAddress { throw StripeError("Simulated write failure") }
            snapshot.values[reg] = Array(command.dropFirst(5))
            if reg == .power && disconnectPower { throw StripeError("Reader restarted") }
        }
        return frame(snapshot.values[reg] ?? [])
    }
}
