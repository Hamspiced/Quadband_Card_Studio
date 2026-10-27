// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public enum MSRProtocol {
    public static let reset: [UInt8] = [0x1B, 0x61]
    public static let ping: [UInt8] = [0x1B, 0x65]
    public static let read: [UInt8] = [0x1B, 0x72]

    public static func write(_ card: CardData, settings: EncodingSettings) throws -> [UInt8] {
        try card.validate(densities: settings.densities)
        var bytes: [UInt8] = [0x1B, 0x77, 0x1B, 0x73]
        for (index, text) in card.normalized.tracks.enumerated() {
            bytes += [0x1B, UInt8(index + 1)] + Array(text.utf8)
        }
        return bytes + [0x3F, 0x1C]
    }

    public static func configuration(_ settings: EncodingSettings) throws -> [[UInt8]] {
        guard settings.densities.count == 3, settings.densities.allSatisfy({ [75, 210].contains($0) }) else {
            throw StripeError("Invalid density settings.")
        }
        // Leading zero command shares one value for Tracks 1 and 3.
        // Using 22 at 75 BPI avoids consuming most of the recording area.
        let lead13: UInt8 = settings.densities[0] == 75 || settings.densities[2] == 75 ? 22 : 61
        let lead2: UInt8 = settings.densities[1] == 75 ? 22 : 61
        return [
            [0x1B, settings.highCoercivity ? 0x78 : 0x79],
            [0x1B, 0x6F, 7, 5, 5],
            [0x1B, 0x62, settings.densities[0] == 210 ? 0xA1 : 0xA0],
            [0x1B, 0x62, settings.densities[1] == 210 ? 0xD2 : 0x4B],
            [0x1B, 0x62, settings.densities[2] == 210 ? 0xC1 : 0xC0],
            [0x1B, 0x7A, lead13, lead2]
        ]
    }
    public static func checkStatus(_ reply: [UInt8]) throws {
        guard reply.count >= 2, reply[0] == 0x1B else { throw StripeError("Unexpected device response.") }
        guard reply[1] == 0x30 else { throw statusError(reply[1]) }
    }
    public static func statusError(_ status: UInt8) -> StripeError {
        let descriptions: [UInt8: String] = [
            0x31: "Read/write error. Swipe steadily and check the card orientation and coercivity.",
            0x32: "The writer rejected the data format.",
            0x34: "The device does not support this command.",
            0x39: "Invalid swipe. Swipe smoothly through the entire slot.",
            0x41: "The device rejected a setting. This firmware may not support the selected format."
        ]
        return StripeError(descriptions[status] ?? String(format: "Device error 0x%02X.", status))
    }
    public static func parseRead(_ bytes: [UInt8]) throws -> CardData {
        if bytes.count == 2 { try checkStatus(bytes); throw StripeError("The device returned no track data.") }
        guard bytes.count >= 6, Array(bytes.prefix(2)) == [0x1B, 0x73],
              Array(bytes.suffix(4).prefix(3)) == [0x3F, 0x1C, 0x1B] else {
            throw StripeError("Incomplete or malformed card response.")
        }
        guard bytes.last == 0x30 else { throw statusError(bytes.last!) }
        let end = bytes.count - 4
        var i = 2
        var result = CardData()
        var seen = Set<Int>()
        while i < end {
            guard i + 1 < end, bytes[i] == 0x1B, (1...3).contains(bytes[i + 1]) else {
                throw StripeError("Invalid track marker in device response.")
            }
            let track = Int(bytes[i + 1]) - 1
            guard !seen.contains(track) else { throw StripeError("Duplicate track in device response.") }
            seen.insert(track)
            i += 2
            let start = i
            while i < end && bytes[i] != 0x1B { i += 1 }
            guard let value = String(bytes: bytes[start..<i], encoding: .ascii) else {
                throw StripeError("The device returned non-ASCII track data.")
            }
            result.tracks[track] = CardData.payload(value, track: track)
        }
        guard !result.isEmpty else { throw StripeError("No readable tracks on this swipe.") }
        return result
    }
}

public enum HIDFraming {
    /// The MSR X-series wraps serial commands in 64-byte HID feature reports.
    public static func packets(_ message: [UInt8]) -> [[UInt8]] {
        guard !message.isEmpty else { return [] }
        return stride(from: 0, to: message.count, by: 63).map { offset in
            let chunk = Array(message[offset..<min(offset + 63, message.count)])
            let header = UInt8(chunk.count) | (offset == 0 ? 0x80 : 0) | (offset + chunk.count == message.count ? 0x40 : 0)
            return [header] + chunk + Array(repeating: 0, count: 63 - chunk.count)
        }
    }
}

public struct HIDAssembler {
    private var buffer: [UInt8] = []
    private var inSequence = false
    public init() {}
    public mutating func reset() { buffer = []; inSequence = false }
    public mutating func accept(_ packet: [UInt8]) throws -> [UInt8]? {
        guard let header = packet.first else { throw StripeError("Empty USB report.") }
        let count = Int(header & 0x3F)
        guard count > 0, packet.count >= count + 1 else { reset(); throw StripeError("Truncated USB report.") }
        if header & 0x80 != 0 {
            guard !inSequence else { reset(); throw StripeError("USB message interrupted before completion.") }
            buffer = []; inSequence = true
        }
        guard inSequence else { throw StripeError("USB report arrived without a message start.") }
        buffer += packet[1...count]
        guard buffer.count <= 4096 else { reset(); throw StripeError("USB response exceeds the supported size.") }
        if header & 0x40 != 0 {
            let result = buffer
            reset()
            return result
        }
        return nil
    }
}

@MainActor public protocol MSRTransport: AnyObject {
    func send(_ message: [UInt8]) throws
    func exchange(_ message: [UInt8], timeout: TimeInterval) async throws -> [UInt8]
    func cancel()
    func close()
}

public extension MSRTransport { func close() {} }

/// Bluetooth and USB serial deliver arbitrary chunks of the serial MSR response.
public struct MSRStreamAssembler {
    private var buffer: [UInt8] = []
    private let command: UInt8
    public init(command: [UInt8]) { self.command = command.count > 1 ? command[1] : 0 }
    public mutating func accept(_ bytes: [UInt8]) throws -> [UInt8]? {
        buffer += bytes
        guard buffer.count <= 4096 else { throw StripeError("Serial response is too large.") }
        guard buffer.count >= 2 else { return nil }
        guard buffer[0] == 0x1B else { throw StripeError("Unexpected Serial response framing.") }
        let length: Int
        if command == 0x72 && buffer[1] == 0x73 {
            guard buffer.count >= 6 else { return nil }
            guard let end = (2..<(buffer.count - 3)).first(where: { Array(buffer[$0..<($0 + 3)]) == [0x3F, 0x1C, 0x1B] }) else { return nil }
            length = end + 4
        } else if command == 0x6F && buffer[1] == 0x30 { length = 5 }
        else { length = 2 }
        guard buffer.count >= length else { return nil }
        guard buffer.count == length else { throw StripeError("Unexpected trailing data in Serial response. Reconnect the writer.") }
        let result = buffer; buffer = []
        return result
    }
}

@MainActor public final class MSRClient {
    public let transport: MSRTransport
    public init(transport: MSRTransport) { self.transport = transport }
    public func handshake() async throws {
        try transport.send(MSRProtocol.reset)
        try await Task.sleep(nanoseconds: 150_000_000)
        let result = try await transport.exchange(MSRProtocol.ping, timeout: 3)
        guard result == [0x1B, 0x79] else { throw StripeError("The connected device did not identify as an MSR-compatible writer.") }
    }
    public func configure(_ settings: EncodingSettings) async throws {
        for command in try MSRProtocol.configuration(settings) {
            try Task.checkCancellation()
            let response = try await transport.exchange(command, timeout: 3)
            try MSRProtocol.checkStatus(response)
            if command[1] == 0x6F, response != [0x1B, 0x30, 7, 5, 5] {
                throw StripeError("The writer did not confirm 7/5/5 bits per character.")
            }
        }
    }
    public func write(_ card: CardData, settings: EncodingSettings) async throws {
        let command = try MSRProtocol.write(card, settings: settings)
        let response = try await transport.exchange(command, timeout: 45)
        try MSRProtocol.checkStatus(response)
    }
    public func read() async throws -> CardData {
        let response = try await transport.exchange(MSRProtocol.read, timeout: 45)
        return try MSRProtocol.parseRead(response)
    }
}
