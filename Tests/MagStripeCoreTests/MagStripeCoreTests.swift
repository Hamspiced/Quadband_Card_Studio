// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
@testable import MagStripeCore

final class MagStripeCoreTests: XCTestCase {
    func testOriginalSampleFitsStandardDensitiesAtTrack2Limit() throws {
        try CardData.originalTest.validate(densities: [210, 75, 210])
        XCTAssertEqual(CardData.originalTest.tracks.map { $0.utf8.count }, [56, 37, 60])
        XCTAssertThrowsError(try CardData.originalTest.validate(densities: [75, 75, 75]))
        try CardData.shortTest.validate(densities: [75, 75, 75])
    }
    func testSentinelsAreRemovedExactlyOnceAndSpacesPreserved() throws {
        let card = CardData(["% MIDWEST TEST ?", ";123=456?", "+789=012?"])
        XCTAssertEqual(card.normalized.tracks, [" MIDWEST TEST ", "123=456", "789=012"])
        try card.validate(densities: [210, 75, 210])
        XCTAssertThrowsError(try CardData(["%%TEST??", "", ""]).validate(densities: [210, 75, 210]))
    }
    func testInvalidPayloadAndOversizeAreRejected() {
        for value in ["lowercase", "CAFÉ", "ABC\u{1B}DEF", "ABC?DEF", "LINE\nBREAK"] {
            XCTAssertThrowsError(try CardData([value, "", ""]).validate(densities: [210, 75, 210]))
        }
        XCTAssertThrowsError(try CardData(["", "123A", ""]).validate(densities: [210, 75, 210]))
        XCTAssertThrowsError(try CardData(["", String(repeating: "1", count: 38), ""]).validate(densities: [210, 75, 210]))
        XCTAssertThrowsError(try CardData().validate(densities: [210, 75, 210]))
    }
    func testWriteMatchesManufacturerWireExample() throws {
        let bytes = try MSRProtocol.write(CardData(["%ABC123?", ";12345?", ";12345?"]), settings: EncodingSettings())
        XCTAssertEqual(bytes, [0x1B,0x77,0x1B,0x73,0x1B,1] + Array("ABC123".utf8) + [0x1B,2] + Array("12345".utf8) + [0x1B,3] + Array("12345".utf8) + [0x3F,0x1C])
    }
    func testReadMatchesManufacturerWireExampleAndTrack2Only() throws {
        let bytes: [UInt8] = [0x1B,0x73,0x1B,1] + Array("%01?".utf8) + [0x1B,2] + Array(";23?".utf8) + [0x1B,3] + Array(";45?".utf8) + [0x3F,0x1C,0x1B,0x30]
        XCTAssertEqual(try MSRProtocol.parseRead(bytes), CardData(["01", "23", "45"]))
        let only2: [UInt8] = [0x1B,0x73,0x1B,1,0x1B,2] + Array(";123?".utf8) + [0x3F,0x1C,0x1B,0x30]
        XCTAssertEqual(try MSRProtocol.parseRead(only2), CardData(["", "123", ""]))
        XCTAssertThrowsError(try MSRProtocol.parseRead(Array(bytes.dropLast())))
        XCTAssertThrowsError(try MSRProtocol.parseRead(Array(bytes.dropLast()) + [0x31]))
        XCTAssertThrowsError(try MSRProtocol.parseRead([0x1B,0x73,0x1B,4,0x3F,0x1C,0x1B,0x30]))
    }
    func testKeyboardReaderTrack3PrefixesAndPerTrackReturns() throws {
        XCTAssertEqual(try KeyboardSwipeParser.parse("%MIDWEST?;123=456?+789=012?\r"), CardData(["MIDWEST", "123=456", "789=012"]))
        XCTAssertEqual(try KeyboardSwipeParser.parse("%MIDWEST?\r\n;123=456?\r;789=012?\r"), CardData(["MIDWEST", "123=456", "789=012"]))
        XCTAssertEqual(try KeyboardSwipeParser.parse(";123?\r"), CardData(["", "123", ""]))
        XCTAssertEqual(try KeyboardSwipeParser.parse("+789?\r"), CardData(["", "", "789"]))
        XCTAssertThrowsError(try KeyboardSwipeParser.parse("%INCOMPLETE"))
        XCTAssertThrowsError(try KeyboardSwipeParser.parse("%ONE?%TWO?"))
        XCTAssertThrowsError(try KeyboardSwipeParser.parse("12345\r"))
    }
    func testHIDBoundaryPacketsAndRoundtrip() throws {
        for count in [1, 2, 62, 63, 64, 126, 127, 240] {
            let input = (0..<count).map { UInt8($0 % 256) }
            let packets = HIDFraming.packets(input)
            XCTAssertTrue(packets.allSatisfy { $0.count == 64 })
            XCTAssertNotEqual(packets.first![0] & 0x80, 0)
            XCTAssertNotEqual(packets.last![0] & 0x40, 0)
            var assembler = HIDAssembler()
            var completed: [UInt8]?
            for packet in packets { completed = try assembler.accept(packet) }
            XCTAssertEqual(completed, input)
        }
        XCTAssertEqual(HIDFraming.packets(MSRProtocol.ping).first?.prefix(3), [0xC2, 0x1B, 0x65])
    }
    func testMalformedAndOutOfOrderUSBReportsFail() throws {
        var a = HIDAssembler()
        XCTAssertThrowsError(try a.accept([0xC3, 1]))
        XCTAssertThrowsError(try a.accept([0x42, 1, 2]))
        XCTAssertNil(try a.accept([0x82, 1, 2]))
        XCTAssertThrowsError(try a.accept([0xC2, 3, 4]))
        XCTAssertEqual(try a.accept([0xC2, 5, 6]), [5, 6])
    }
    func testDensityCommandsAndLeadingZeros() throws {
        let standard = try MSRProtocol.configuration(EncodingSettings())
        XCTAssertEqual(standard, [[0x1B,0x78], [0x1B,0x6F,7,5,5], [0x1B,0x62,0xA1], [0x1B,0x62,0x4B], [0x1B,0x62,0xC1], [0x1B,0x7A,61,22]])
        let low = try MSRProtocol.configuration(EncodingSettings(densities: [75,75,75], highCoercivity: false))
        XCTAssertEqual(low[0], [0x1B,0x79]); XCTAssertEqual(low[2], [0x1B,0x62,0xA0]); XCTAssertEqual(low[4], [0x1B,0x62,0xC0])
        XCTAssertEqual(low[5], [0x1B,0x7A,22,22])
    }
    @MainActor func testConfigurationFailurePreventsLaterCommands() async throws {
        let transport = MockTransport(responses: [[0x1B,0x30], [0x1B,0x34]])
        let client = MSRClient(transport: transport)
        do { try await client.configure(EncodingSettings()); XCTFail("Expected rejection") }
        catch { XCTAssertEqual(transport.sent.count, 2) }
    }
    @MainActor func testHandshakeRejectsUnknownDevice() async {
        let transport = MockTransport(responses: [[0x1B,0x30]])
        do { try await MSRClient(transport: transport).handshake(); XCTFail("Expected rejection") }
        catch { XCTAssertEqual(transport.sent, [MSRProtocol.reset, MSRProtocol.ping]) }
    }
    @MainActor func testSimulatedConfigureWriteReadCycle() async throws {
        let wire: [UInt8] = [0x1B,0x73,0x1B,1] + Array("%ABC?".utf8) + [0x1B,2] + Array(";123?".utf8) + [0x1B,3] + Array(";456?".utf8) + [0x3F,0x1C,0x1B,0x30]
        let transport = MockTransport(responses: [[0x1B,0x79], [0x1B,0x30], [0x1B,0x30,7,5,5], [0x1B,0x30], [0x1B,0x30], [0x1B,0x30], [0x1B,0x30], [0x1B,0x30], wire])
        let client = MSRClient(transport: transport)
        try await client.handshake()
        try await client.configure(EncodingSettings())
        let expected = CardData(["ABC", "123", "456"])
        try await client.write(expected, settings: EncodingSettings())
        let read = try await client.read()
        XCTAssertEqual(read, expected)
        XCTAssertTrue(transport.responses.isEmpty)
        XCTAssertEqual(transport.sent.last, MSRProtocol.read)
    }
}

@MainActor private final class MockTransport: MSRTransport {
    var responses: [[UInt8]]
    var sent: [[UInt8]] = []
    init(responses: [[UInt8]]) { self.responses = responses }
    func send(_ message: [UInt8]) throws { sent.append(message) }
    func exchange(_ message: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        sent.append(message)
        guard !responses.isEmpty else { throw StripeError("No simulated response") }
        return responses.removeFirst()
    }
    func cancel() {}
}
