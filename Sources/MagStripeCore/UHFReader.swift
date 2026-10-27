// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public enum UHFReaderProfile: String, CaseIterable, Identifiable {
    case piswords = "Piswords UHF2"
    case jrd4035 = "M5Stack JRD-4035"
    case yrm100 = "YRM100"
    public var id: String { rawValue }
    public var isM100: Bool { self != .piswords }
    public var defaultBaud: Int { isM100 ? 115200 : 9600 }
    public var powerRange: ClosedRange<Int> { isM100 ? 18...26 : 0...30 }
    public var maxWriteBytes: Int { isM100 ? 64 : 128 }
    public var maxEPCBytes: Int { isM100 ? 28 : 62 }
}

public struct UHFReaderSettings {
    public let details: String
    public let power: Int?
    public init(details: String, power: Int?) { self.details = details; self.power = power }
}

@MainActor public protocol UHFReaderClient: AnyObject {
    var onInventoryError: ((Error) -> Void)? { get set }
    func identify() async throws -> String
    func settings() async throws -> UHFReaderSettings
    func observe(_ tag: UHFTag)
    func startInventory() async throws
    func stopInventory() async throws
    func read(epc: [UInt8], bank: UHFMemoryBank, word: UInt32, count: Int, password: [UInt8]) async throws -> UHFMemoryRead
    func writeUser(epc: [UInt8], word: UInt32, bytes: [UInt8], password: [UInt8]) async throws -> UHFMemoryRead
    func changeEPC(from old: [UInt8], to new: [UInt8], password: [UInt8]) async throws -> UHFMemoryRead
    func setBuzzer(_ enabled: Bool) async throws
    func setPower(_ value: Int) async throws
    func close()
}

extension UHFClient: UHFReaderClient {
    public func identify() async throws -> String {
        try UHFProtocol.version(await transport.exchange(0x72, data: [], timeout: 0.8))
    }
    public func observe(_ tag: UHFTag) {}
    public func settings() async throws -> UHFReaderSettings {
        var pieces: [String] = []
        let ant = try await command(0x75)
        if ant.count == 1 && (1...8).contains(ant[0]) { pieces.append("Antenna \(ant[0])") }
        let rf = try await command(0x77)
        let validPower = [1,4,8].contains(rf.count) && rf.allSatisfy { $0 <= 33 }
        if validPower { pieces.append("\(rf.map(String.init).joined(separator: " / ")) dBm") }
        let region = try await command(0x79)
        if region.count >= 3 { pieces.append("Region \(region[0]) · channels \(region[1])–\(region[2])") }
        return UHFReaderSettings(details: pieces.joined(separator: "  ·  "), power: validPower ? Int(rf[0]) : nil)
    }
    public func startInventory() async throws {
        try UHFProtocol.acknowledge(await command(0x85, [1]))
        try UHFProtocol.acknowledge(await command(0x89, [1]))
    }
    public func stopInventory() async throws { try UHFProtocol.acknowledge(await command(0x8C)) }
    public func writeUser(epc: [UInt8], word: UInt32, bytes: [UInt8], password: [UInt8]) async throws -> UHFMemoryRead {
        try await write(epc: epc, bank: .user, word: word, bytes: bytes, password: password)
    }
    public func setBuzzer(_ enabled: Bool) async throws {
        try UHFProtocol.acknowledge(await command(0x7A, [enabled ? 2 : 0]))
    }
    public func setPower(_ value: Int) async throws {
        guard (0...30).contains(value) else { throw StripeError("Piswords power must be 0–30 dBm.") }
        try UHFProtocol.acknowledge(await command(0x76, [UInt8(value)]))
        guard try await command(0x77) == [UInt8(value)] else { throw StripeError("Power did not match on read-back.") }
    }
    public func close() { transport.close() }
}
