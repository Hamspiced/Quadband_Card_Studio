// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct M100Frame: Equatable {
    public let kind: UInt8
    public let command: UInt8
    public let data: [UInt8]
    public init(kind: UInt8 = 0, command: UInt8, data: [UInt8] = []) {
        self.kind = kind; self.command = command; self.data = data
    }
    public func encoded() throws -> [UInt8] {
        guard kind <= 2, data.count <= 1024 else { throw StripeError("Invalid M100 frame type or payload length.") }
        let body = [kind, command, UInt8(data.count >> 8), UInt8(truncatingIfNeeded: data.count)] + data
        return [0xBB] + body + [body.reduce(0, &+), 0x7E]
    }
}

public struct M100FrameDecoder {
    private var bytes: [UInt8] = []
    public init() {}
    public mutating func accept(_ chunk: [UInt8]) throws -> [M100Frame] {
        bytes += chunk
        guard bytes.count <= 65536 else { bytes = []; throw StripeError("M100 response buffer overflow. Reconnect.") }
        var frames: [M100Frame] = []
        while !bytes.isEmpty {
            guard bytes[0] == 0xBB else { bytes = []; throw StripeError("Unexpected M100 data. Check profile, port and baud.") }
            guard bytes.count >= 5 else { break }
            let length = Int(bytes[3]) << 8 | Int(bytes[4])
            guard [1,2].contains(bytes[1]), length <= 1024 else { bytes = []; throw StripeError("Invalid M100 response type or length.") }
            guard bytes.count >= length + 7 else { break }
            let packet = Array(bytes.prefix(length + 7))
            guard packet.last == 0x7E, packet[1..<packet.count-2].reduce(UInt8(0), &+) == packet[packet.count-2] else {
                bytes = []; throw StripeError("M100 response checksum or terminator failed. Reconnect.")
            }
            bytes.removeFirst(packet.count)
            frames.append(M100Frame(kind: packet[1], command: packet[2], data: Array(packet[5..<packet.count-2])))
        }
        return frames
    }
}

public enum M100Protocol {
    public static func crc16(_ bytes: [UInt8]) -> UInt16 {
        var crc: UInt16 = 0xFFFF
        for byte in bytes {
            crc ^= UInt16(byte) << 8
            for _ in 0..<8 { crc = (crc &<< 1) ^ (crc & 0x8000 != 0 ? 0x1021 : 0) }
        }
        return crc ^ 0xFFFF
    }
    public static func error(_ data: [UInt8]) -> StripeError {
        let code = data.first ?? 0
        let messages: [UInt8: String] = [0x09: "No matching tag for read", 0x10: "Tag write failed",
            0x15: "No tag found", 0x16: "Incorrect access password", 0x17: "Invalid command parameters",
            0x20: "RF power setting failed", 0xA3: "Read exceeds tag memory", 0xB3: "Write exceeds tag memory",
            0xA4: "Tag memory locked", 0xB4: "Tag memory locked"]
        return StripeError((messages[code] ?? "M100 reader rejected command") + String(format: " (0x%02X).", code))
    }
    public static func acknowledge(_ data: [UInt8]) throws {
        guard data == [0] else { throw StripeError("M100 reader did not acknowledge the command.") }
    }
    public static func version(_ data: [UInt8]) throws -> String {
        guard (2...129).contains(data.count), data[0] == 0, data.dropFirst().allSatisfy({ (32..<127).contains($0) }) else {
            throw StripeError("Invalid M100 hardware identification.")
        }
        let value = String(decoding: data.dropFirst(), as: UTF8.self)
        guard ["M100", "QM100", "JRD", "YRM"].contains(where: { value.uppercased().contains($0) }) else {
            throw StripeError("Reader did not identify as an M100-family UHF module.")
        }
        return value
    }
    public static func tag(_ data: [UInt8]) throws -> UHFTag {
        guard data.count >= 7 else { throw StripeError("Incomplete M100 tag report.") }
        let length = Int(data[1] >> 3) * 2
        guard (2...62).contains(length), data.count == length + 5 else { throw StripeError("Invalid M100 EPC report length.") }
        let crc = UInt16(data[data.count-2]) << 8 | UInt16(data[data.count-1])
        guard crc16(Array(data[1..<data.count-2])) == crc else { throw StripeError("M100 tag CRC failed.") }
        return UHFTag(epc: Array(data[3..<data.count-2]), pc: Array(data[1...2]), signal: Int(Int8(bitPattern: data[0])))
    }
    public static func selection(epc: [UInt8], pc: [UInt8]) throws -> [UInt8] {
        _ = try UHFProtocol.selection(epc)
        guard epc.count <= 28 else { throw StripeError("M100 exact selection supports EPCs up to 28 bytes; longer EPCs are inventory-only.") }
        guard pc.count == 2, Int(pc[0] >> 3)*2 == epc.count else { throw StripeError("Scan the tag again to obtain its current PC word.") }
        let mask = pc + epc
        return [1,0,0,0,0x10,UInt8(mask.count*8),0] + mask
    }
    public static func readCommand(bank: UHFMemoryBank, word: UInt32, count: Int, password: [UInt8]) throws -> [UInt8] {
        guard word <= 0xFFFF, (1...64).contains(count), UInt64(word)+UInt64(count) <= 0x10000, password.count == 4 else {
            throw StripeError("M100 reads require 1–64 words within a 16-bit word address range and a four-byte password.")
        }
        return password + [UInt8(bank.rawValue),UInt8(word >> 8),UInt8(truncatingIfNeeded: word),0,UInt8(count)]
    }
    public static func writeCommand(bank: UHFMemoryBank, word: UInt32, bytes: [UInt8], password: [UInt8]) throws -> [UInt8] {
        guard [.epc,.user].contains(bank), bank != .epc || word >= 1, (2...64).contains(bytes.count), bytes.count % 2 == 0 else {
            throw StripeError("M100 writes support 1–32 words to User memory or EPC after its CRC word.")
        }
        return try readCommand(bank: bank, word: word, count: bytes.count/2, password: password) + bytes
    }
    public static func memory(_ data: [UInt8], epc: [UInt8], pc: [UInt8], size: Int) throws -> UHFMemoryRead {
        guard data.count >= 5, Int(data[0]) == epc.count+2, data.count == 1+epc.count+2+size else {
            throw StripeError("Invalid M100 memory response length.")
        }
        guard Array(data[1...2]) == pc, Array(data[3..<3+epc.count]) == epc else {
            throw StripeError("Reader returned a different tag or changed PC word.")
        }
        return UHFMemoryRead(epc: epc, pc: pc, bytes: Array(data.suffix(size)), antenna: 1)
    }
}

@MainActor public final class M100Client: UHFReaderClient {
    public let transport: any UHFTransport
    public var onInventoryError: ((Error) -> Void)?
    private let profile: UHFReaderProfile
    private var seen: [[UInt8]: [UInt8]] = [:]
    private var inventoryActive = false
    private var inventoryTask: Task<Void, Never>?
    public init(transport: any UHFTransport, profile: UHFReaderProfile = .jrd4035) {
        self.transport = transport; self.profile = profile
    }
    public func command(_ command: UInt8, _ data: [UInt8] = [], timeout: TimeInterval = 3) async throws -> [UInt8] {
        try await transport.exchange(command, data: data, timeout: timeout)
    }
    public func identify() async throws -> String {
        let version = try M100Protocol.version(await command(0x03, [0], timeout: 0.9))
        return "\(profile.rawValue) · \(version)"
    }
    public func settings() async throws -> UHFReaderSettings {
        let rf = try await command(0xB7)
        guard rf.count == 2 else { throw StripeError("Invalid M100 power response.") }
        let value = Int(rf[0]) << 8 | Int(rf[1])
        let region = try await command(0x08)
        guard region.count == 1 else { throw StripeError("Invalid M100 region response.") }
        return UHFReaderSettings(details: String(format: "%.2f dBm · region %d · frequency not reported per tag", Double(value)/100, region[0]), power: value/100)
    }
    public func observe(_ tag: UHFTag) {
        if seen[tag.epc] != nil || seen.count < 2000 { seen[tag.epc] = tag.pc }
    }
    private func select(_ epc: [UInt8]) async throws -> [UInt8] {
        guard let pc = seen[epc] else { throw StripeError("Scan and select the tag before accessing its memory.") }
        let payload = try M100Protocol.selection(epc: epc, pc: pc)
        try M100Protocol.acknowledge(await command(0x0C, payload))
        guard try await command(0x0B) == payload else { throw StripeError("M100 exact PC/EPC selection did not match. No write sent.") }
        try M100Protocol.acknowledge(await command(0x12, [0]))
        return pc
    }
    public func read(epc: [UInt8], bank: UHFMemoryBank, word: UInt32, count: Int, password: [UInt8]) async throws -> UHFMemoryRead {
        let payload = try M100Protocol.readCommand(bank: bank, word: word, count: count, password: password)
        let pc = try await select(epc)
        return try M100Protocol.memory(await command(0x39, payload), epc: epc, pc: pc, size: count*2)
    }
    private func write(epc: [UInt8], bank: UHFMemoryBank, word: UInt32, bytes: [UInt8], password: [UInt8], newEPC: [UInt8]? = nil, expectedPC: [UInt8]? = nil) async throws -> UHFMemoryRead {
        let payload = try M100Protocol.writeCommand(bank: bank, word: word, bytes: bytes, password: password)
        if let newEPC { _ = try M100Protocol.selection(epc: newEPC, pc: Array(bytes.prefix(2))) }
        let current = try await read(epc: epc, bank: .epc, word: 1, count: 1, password: password)
        guard current.bytes == seen[epc], expectedPC == nil || current.bytes == expectedPC else {
            throw StripeError("Tag control word changed. Scan/read again before writing.")
        }
        try Task.checkCancellation()
        do {
            let response = try await command(0x49, payload)
            guard response.count >= 5 else { throw StripeError("Incomplete M100 write result.") }
            let reportedEPC = Array(response[3..<response.count-1])
            guard reportedEPC == epc || reportedEPC == newEPC else { throw StripeError("Write result identified a different EPC.") }
            let pc = newEPC != nil && reportedEPC == newEPC ? Array(bytes.prefix(2)) : current.bytes
            let result = try M100Protocol.memory(response, epc: reportedEPC, pc: pc, size: 1)
            try M100Protocol.acknowledge(result.bytes)
            if let newEPC { seen[newEPC] = Array(bytes.prefix(2)) }
            let verified = try await read(epc: newEPC ?? epc, bank: bank, word: word, count: bytes.count/2, password: password)
            guard verified.bytes == bytes else { throw StripeError("M100 read-back differs from the requested data.") }
            return verified
        } catch {
            throw StripeError("Write not verified; the tag may be partly changed. Scan/read before retrying. \(error.localizedDescription)")
        }
    }
    public func writeUser(epc: [UInt8], word: UInt32, bytes: [UInt8], password: [UInt8]) async throws -> UHFMemoryRead {
        try await write(epc: epc, bank: .user, word: word, bytes: bytes, password: password)
    }
    public func changeEPC(from old: [UInt8], to new: [UInt8], password: [UInt8]) async throws -> UHFMemoryRead {
        _ = try UHFProtocol.selection(new)
        let current = try await read(epc: old, bank: .epc, word: 1, count: 1, password: password)
        let bytes = try UHFProtocol.epcPayload(new, currentPC: current.bytes)
        _ = try M100Protocol.selection(epc: new, pc: Array(bytes.prefix(2)))
        return try await write(epc: old, bank: .epc, word: 1, bytes: bytes, password: password, newEPC: new, expectedPC: current.bytes)
    }
    public func startInventory() async throws {
        guard !inventoryActive else { throw StripeError("M100 inventory is already running.") }
        try M100Protocol.acknowledge(await command(0x12, [1]))
        inventoryActive = true
        inventoryTask = Task { [weak self] in
            guard let self else { return }
            do {
                while self.inventoryActive && !Task.isCancelled {
                    _ = try await self.command(0x22, timeout: 1.5)
                    if self.inventoryActive { try await Task.sleep(nanoseconds: 100_000_000) }
                }
            } catch {
                if self.inventoryActive { self.inventoryActive = false; self.transport.close(); self.onInventoryError?(error) }
            }
        }
    }
    public func stopInventory() async throws {
        inventoryActive = false
        await inventoryTask?.value
        inventoryTask = nil
        try M100Protocol.acknowledge(await command(0x28))
    }
    public func setBuzzer(_ enabled: Bool) async throws { throw StripeError("JRD-4035/YRM100 has no documented module buzzer command.") }
    public func setPower(_ value: Int) async throws {
        guard (18...26).contains(value) else { throw StripeError("Module power controls are limited to 18–26 dBm.") }
        let raw = value*100, data = [UInt8(raw >> 8),UInt8(raw & 255)]
        try M100Protocol.acknowledge(await command(0xB6, data))
        guard try await command(0xB7) == data else { throw StripeError("M100 power did not match on read-back.") }
    }
    public func close() {
        inventoryActive = false; inventoryTask?.cancel(); inventoryTask = nil; transport.close()
    }
}
