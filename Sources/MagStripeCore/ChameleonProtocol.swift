// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct ChameleonFrame: Equatable {
    public let command: UInt16
    public let status: UInt16
    public let data: [UInt8]
    public init(command: UInt16, status: UInt16 = 0, data: [UInt8] = []) { self.command = command; self.status = status; self.data = data }
    public func encoded() throws -> [UInt8] {
        guard data.count <= 4096 else { throw StripeError("Chameleon packet is too large.") }
        var bytes: [UInt8] = [0x11,0xEF,UInt8(command >> 8),UInt8(command & 255),UInt8(status >> 8),UInt8(status & 255),UInt8(data.count >> 8),UInt8(data.count & 255)]
        bytes.append(0 &- bytes.reduce(0,&+)); bytes += data; bytes.append(0 &- bytes.reduce(0,&+)); return bytes
    }
}
public struct ChameleonDecoder {
    private var buffer: [UInt8] = []
    public init() {}
    public mutating func accept(_ bytes: [UInt8]) throws -> [ChameleonFrame] {
        buffer += bytes
        var result: [ChameleonFrame] = []
        while !buffer.isEmpty {
            guard buffer[0] == 0x11 else { throw StripeError("Invalid Chameleon packet start. Reconnect the reader.") }
            if buffer.count < 9 { break }
            guard buffer[1] == 0xEF, buffer.prefix(9).reduce(0,&+) == 0 else { throw StripeError("Chameleon header checksum failed.") }
            let count = Int(buffer[6])*256+Int(buffer[7])
            guard count <= 4096 else { throw StripeError("Invalid Chameleon packet length.") }
            if buffer.count < count+10 { break }
            guard buffer.prefix(count+10).reduce(0,&+) == 0 else { throw StripeError("Chameleon response checksum failed.") }
            result.append(ChameleonFrame(command:UInt16(buffer[2])*256+UInt16(buffer[3]),status:UInt16(buffer[4])*256+UInt16(buffer[5]),data:Array(buffer[9..<9+count])))
            buffer.removeFirst(count+10)
        }
        return result
    }
}

public enum LFFormat: String, CaseIterable, Codable, Identifiable {
    case em410x = "EM410x", electra = "EM410x Electra", hid = "HID Prox", viking = "Viking", ioprox = "ioProx", pac = "PAC / Stanley", jablotron = "Jablotron", pyramid = "Pyramid"
    public var id: String { rawValue }
    public var scanCommand: UInt16 { switch self { case .em410x,.electra: return 3000; case .hid: return 3002; case .viking: return 3004; case .ioprox: return 3010; case .pac: return 3014; case .jablotron: return 3019; case .pyramid: return 3021 } }
    public var size: Int { switch self { case .em410x,.jablotron: return 5; case .electra,.hid: return 13; case .viking: return 4; case .ioprox,.pyramid: return 16; case .pac: return 8 } }
}
public struct LFCard: Codable, Equatable {
    public let format: LFFormat
    public let bytes: [UInt8]
    public init(format: LFFormat, bytes: [UInt8]) throws {
        guard bytes.count == format.size else { throw StripeError("Invalid \(format.rawValue) card length.") }
        if format == .hid { guard (1...30).contains(bytes[0]) || bytes[0] == 42 else { throw StripeError("HID format is unknown. Choose the correct HID format and scan again before cloning.") } }
        if format == .pac { guard bytes.allSatisfy({ (0x20...0x7E).contains($0) }) else { throw StripeError("Invalid PAC card ID.") } }
        self.format = format; self.bytes = bytes
    }
    public var identifier: String { format == .pac ? String(decoding:bytes,as:UTF8.self) : HexBytes.string(bytes).replacingOccurrences(of:" ",with:"") }
    public var details: String {
        func number(_ range: Range<Int>) -> UInt64 { bytes[range].reduce(0) { ($0 << 8) | UInt64($1) } }
        switch format {
        case .hid: return "Format \(bytes[0]) · Facility \(number(1..<5)) · Card \(number(5..<10)) · Issue \(bytes[10]) · OEM \(number(11..<13))"
        case .ioprox: return "Version \(bytes[0]) · Facility \(bytes[1]) · Card \(number(2..<4))"
        default: return "\(format.rawValue) · \(bytes.count) bytes"
        }
    }
    public static func decode(_ data: [UInt8], format: LFFormat) throws -> LFCard {
        if format == .em410x || format == .electra {
            // Older 2.x firmware returns five bytes; newer firmware prefixes the tag type.
            if data.count == 5 { return try LFCard(format:.em410x,bytes:data) }
            if data.count == 7, [100,101,102,103].contains(Int(data[0])*256+Int(data[1])) { return try LFCard(format:.em410x,bytes:Array(data.dropFirst(2))) }
            if data.count == 15, data.prefix(2) == [0,104] { return try LFCard(format:.electra,bytes:Array(data.dropFirst(2))) }
            throw StripeError("Unknown EM410x response format. Card data was not accepted.")
        }
        if format == .hid, data.count == 16 { return try LFCard(format:format,bytes:Array(data.prefix(13))) }
        return try LFCard(format:format,bytes:data)
    }
    public func matches(_ other: LFCard) -> Bool {
        guard format == other.format else { return false }
        // ioProx's last four bytes are firmware padding, not card data.
        return format == .ioprox ? bytes.prefix(12) == other.bytes.prefix(12) : bytes == other.bytes
    }
    public func fileData() throws -> Data { try JSONEncoder().encode(LFCardFile(schema:1,kind:"Quadband LF card",card:self)) }
    public static func fromFile(_ data: Data) throws -> LFCard {
        guard data.count <= 16_384 else { throw StripeError("LF card file is too large.") }
        let file = try JSONDecoder().decode(LFCardFile.self,from:data)
        guard file.schema == 1, file.kind == "Quadband LF card" else { throw StripeError("Unsupported LF card file.") }
        return try LFCard(format:file.card.format,bytes:file.card.bytes)
    }
}
private struct LFCardFile: Codable { let schema: Int; let kind: String; let card: LFCard }

public struct ChameleonInfo {
    public let version: String
    public let capabilities: Set<UInt16>
    public let originalMode: UInt8
    public var formats: [LFFormat] { LFFormat.allCases.filter { $0 != .electra && capabilities.contains($0.scanCommand) } }
    public func canWrite(_ card: LFCard) -> Bool { capabilities.contains(3016) && capabilities.contains(card.format.scanCommand) && T5577.formats.contains(card.format) }
}
@MainActor public protocol ChameleonTransport: AnyObject {
    func exchange(_ command: UInt16, data: [UInt8], timeout: TimeInterval) async throws -> ChameleonFrame
    func close()
}
@MainActor public final class ChameleonClient {
    private let transport: ChameleonTransport
    public private(set) var info: ChameleonInfo?
    public init(transport: ChameleonTransport) { self.transport = transport }
    public func command(_ command: UInt16, _ data: [UInt8] = [], timeout: TimeInterval = 3) async throws -> ChameleonFrame {
        try Task.checkCancellation()
        try T5577.validateCommand(command,data:data)
        let frame = try await transport.exchange(command,data:data,timeout:timeout)
        guard frame.command == command else { throw StripeError("Unexpected Chameleon response.") }
        return frame
    }
    @discardableResult private func success(_ command: UInt16, _ data: [UInt8] = []) async throws -> [UInt8] {
        let frame = try await self.command(command,data)
        guard frame.status == 0x68 else { throw Self.error(frame.status) }; return frame.data
    }
    public func identify() async throws -> ChameleonInfo {
        let version = try await success(1000)
        guard version.count == 2, version[0] == 2 else { throw StripeError("This Chameleon firmware protocol is unsupported. Install compatible official 2.x firmware.") }
        let model = try await success(1033)
        guard model == [0] else { throw StripeError("LF reading and writing require Chameleon Ultra. Chameleon Lite has no reader hardware.") }
        let list = try await success(1035)
        guard !list.isEmpty, list.count % 2 == 0 else { throw StripeError("Invalid Chameleon capability list.") }
        let capabilities = Set(stride(from:0,to:list.count,by:2).map { UInt16(list[$0])*256+UInt16(list[$0+1]) })
        guard capabilities.contains(1001), capabilities.contains(1002) else { throw StripeError("Reader mode is unavailable in this firmware.") }
        let mode = try await success(1002)
        guard mode.count == 1, mode[0] <= 1 else { throw StripeError("Invalid Chameleon device mode.") }
        var label = "Firmware \(version[0]).\(version[1])"
        if capabilities.contains(1017) {
            let git = try await success(1017)
            if let revision = String(bytes:git,encoding:.utf8), revision.count <= 80 { label += " · \(revision)" }
        }
        let value = ChameleonInfo(version:label,capabilities:capabilities,originalMode:mode[0]); info = value; return value
    }
    public func restoreMode() async throws { if let info { _ = try await success(1001,[info.originalMode]) } }
    public func prepareReader() async throws {
        guard info != nil else { throw StripeError("Connect and identify the Chameleon first.") }
        _ = try await success(1001,[1])
    }
    public func scan(format: LFFormat? = nil, hidHint: UInt8 = 0) async throws -> LFCard? {
        guard let info else { throw StripeError("Connect the Chameleon first.") }
        try await prepareReader()
        let formats = format.map { [$0] } ?? info.formats
        for format in formats {
            guard info.capabilities.contains(format.scanCommand) else { throw StripeError("\(format.rawValue) scanning is unavailable in this firmware.") }
            let reply = try await command(format.scanCommand,format == .hid || format == .ioprox ? [format == .hid ? hidHint : 0] : [],timeout:5)
            if reply.status == 0x41 { continue }
            guard reply.status == 0x40 else { throw Self.error(reply.status) }
            return try LFCard.decode(reply.data,format:format)
        }
        return nil
    }
    public func writeAndVerify(_ card: LFCard) async throws -> LFCard {
        guard let info, info.canWrite(card) else { throw StripeError("Password-free writing requires firmware with command 3016 and a supported encoder. Legacy password-setting writers are disabled.") }
        let words = try T5577.blocks(card)
        try await prepareReader()
        do {
            for (block,word) in words {
                let reply = try await command(3016,T5577.payload(block:block,word:word),timeout:5)
                guard reply.status == 0x40, reply.data.isEmpty else { throw Self.error(reply.status) }
            }
            try await Task.sleep(nanoseconds:250_000_000)
            guard let read = try await scan(format:card.format), card.matches(read) else {
                throw StripeError("The card did not read back with the expected ID.")
            }
            return read
        } catch {
            throw StripeError("Write not verified: \(error.localizedDescription) The target may have changed. Scan it before trying again.")
        }
    }
    public static func error(_ status: UInt16) -> StripeError {
        switch status {
        case 0x41: return StripeError("No supported LF card found. Center one card on the LF antenna.")
        case 0x60: return StripeError("The firmware rejected the command parameters.")
        case 0x66: return StripeError("Chameleon is not in reader mode.")
        case 0x67,0x69: return StripeError("This command is unavailable in the Chameleon firmware.")
        default: return StripeError(String(format:"Chameleon returned status 0x%04X.",status))
        }
    }
}
