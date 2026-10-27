// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct UHFFrame: Equatable {
    public let address: UInt8
    public let command: UInt8
    public let data: [UInt8]
    public init(address: UInt8, command: UInt8, data: [UInt8] = []) { self.address = address; self.command = command; self.data = data }
    public func encoded() throws -> [UInt8] {
        guard data.count <= 252 else { throw StripeError("UHF command exceeds the reader's packet size.") }
        let bytes: [UInt8] = [0xA0,UInt8(data.count+3),address,command] + data
        return bytes + [0 &- bytes.reduce(0,&+)]
    }
}

public struct UHFFrameDecoder {
    private var bytes: [UInt8] = []
    public init() {}
    public mutating func accept(_ chunk: [UInt8]) throws -> [UHFFrame] {
        bytes += chunk
        guard bytes.count <= 65536 else { bytes = []; throw StripeError("UHF response buffer overflow. Reconnect the reader.") }
        var frames: [UHFFrame] = []
        while !bytes.isEmpty {
            guard bytes[0] == 0xA0 else { bytes = []; throw StripeError("Unexpected UHF data. Check the baud rate and selected port.") }
            guard bytes.count >= 2 else { break }
            let length = Int(bytes[1])+2
            guard length >= 5 else { bytes = []; throw StripeError("Invalid UHF packet length.") }
            guard bytes.count >= length else { break }
            let packet = Array(bytes.prefix(length)); bytes.removeFirst(length)
            guard packet.reduce(UInt8(0),&+) == 0 else { bytes = []; throw StripeError("UHF packet checksum failed. Reconnect before retrying.") }
            frames.append(UHFFrame(address:packet[2],command:packet[3],data:Array(packet[4..<length-1])))
        }
        return frames
    }
}

public enum UHFMemoryBank: Int, CaseIterable {
    case reserved = 0, epc = 1, tid = 2, user = 3
    public var name: String { ["Reserved / passwords","EPC","TID / chip ID","User memory"][rawValue] }
}
public struct UHFTag: Identifiable, Equatable {
    public var id: String { HexBytes.string(epc) }
    public var epc: [UInt8]
    public var pc: [UInt8]
    public var antenna: Int
    public var frequencyKHz: Int
    public var signal: Int?
    public var sightings: Int = 1
    public var lastSeen = Date()
    public init(epc: [UInt8], pc: [UInt8], antenna: Int = 1, frequencyKHz: Int = 0, signal: Int? = nil) { self.epc = epc; self.pc = pc; self.antenna = antenna; self.frequencyKHz = frequencyKHz; self.signal = signal }
}
public struct UHFMemoryRead: Equatable {
    public let epc: [UInt8]
    public let pc: [UInt8]
    public let bytes: [UInt8]
    public let antenna: Int
}

/// Piswords UHF2 / UCM60x protocol, as specified by the supplied manufacturer's Java sources.
public enum UHFProtocol {
    public static func error(_ code: UInt8) -> StripeError {
        let descriptions: [UInt8:String] = [0x11:"Reader operation failed",0x22:"Antenna is not connected",0x25:"RF power could not be set",0x31:"Tag scan failed",0x32:"Tag could not be read",0x33:"Tag could not be written",0x36:"No matching UHF tag found",0x37:"Tag found, but memory access failed",0x40:"Access denied or incorrect access password",0x41:"Command is unsupported",0x43:"Memory length is outside the tag's capacity",0x44:"Unsupported memory bank",0x47:"Antenna number is out of range",0x48:"Power is outside the reader's range",0x4C:"EPC selection is too long",0x4D:"Invalid EPC selection length",0x63:"Tag memory is locked",0x64:"Incorrect tag password",0x67:"Reader address does not match"]
        return StripeError((descriptions[code] ?? "Reader returned an error") + String(format:" (0x%02X).",code))
    }
    public static func acknowledge(_ data: [UInt8]) throws {
        guard data == [0x10] else { if data.count == 1 { throw error(data[0]) }; throw StripeError("Reader did not acknowledge the UHF command.") }
    }
    public static func version(_ data: [UInt8]) throws -> String {
        guard data.count == 3, [1,2,6,8,10].contains(data[2]) else { throw StripeError("This device did not identify as a Piswords UHF2-compatible UCM60x reader.") }
        return "\(data[2] == 10 ? "UCM606L" : "UCM60\(data[2])") · firmware \(data[0]).\(data[1])"
    }
    public static func tag(_ data: [UInt8]) throws -> UHFTag {
        guard data.count >= 12 else { throw StripeError("Incomplete UHF tag report.") }
        let length = Int(data[1] >> 3)*2
        guard length > 0, data.count == length+10 else { throw StripeError("UHF tag report has an invalid EPC length.") }
        let frequency = data.suffix(3).reduce(0) { ($0 << 8) | Int($1) }
        guard (840000...960000).contains(frequency), (1...8).contains(data[0]) else { throw StripeError("Invalid UHF frequency or antenna in tag report.") }
        let raw = data[(3+length)..<(7+length)].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        let hardware = Int((raw >> 25) & 15), mode = Int(raw >> 29)
        let b = [[43,43,45,49,43,43,45,49],[43,43,45,49,43,43,45,49],[43,43,45,49,43,43,45,49],[53,53,48,43,49,45,43,43],[47,47,47,47,46,43,43,43]]
        let c = [[43,43,45,49,43,43,45,49],[43,43,45,49,43,43,45,49],[43,43,45,49,43,43,45,49],Array(repeating:-283,count:8),[-303,-283,-253,-238,-304,-313,-280,-266]]
        let amplitude = Int(raw & 0x01FFFFFF)/length
        let signal = hardware < b.count && amplitude > 0 ? Int(Double(b[hardware][mode])*log10(Double(amplitude))+Double(c[hardware][mode])) : nil
        return UHFTag(epc:Array(data[3..<3+length]),pc:Array(data[1...2]),antenna:Int(data[0]),frequencyKHz:frequency,signal:signal)
    }
    public static func readCommand(bank: UHFMemoryBank, word: UInt32, count: Int, password: [UInt8]) throws -> [UInt8] {
        guard (1...64).contains(count), password.count == 4, UInt64(word)+UInt64(count) <= UInt64(UInt32.max) else { throw StripeError("Read 1–64 words with a four-byte access password and valid starting word.") }
        return [UInt8(bank.rawValue)] + bigEndian(word) + [UInt8(count >> 8),UInt8(count & 255)] + password
    }
    public static func writeCommand(bank: UHFMemoryBank, word: UInt32, bytes: [UInt8], password: [UInt8]) throws -> [UInt8] {
        guard [.epc,.user].contains(bank), !bytes.isEmpty, bytes.count <= 128, bytes.count % 2 == 0, password.count == 4 else { throw StripeError("Write 1–64 complete words to EPC or User memory with a four-byte access password.") }
        guard bank != .epc || word >= 1 else { throw StripeError("The EPC CRC word is managed by the tag.") }
        let count = bytes.count/2
        guard UInt64(word)+UInt64(count) <= UInt64(UInt32.max) else { throw StripeError("Memory range is too large.") }
        return password + [UInt8(bank.rawValue)] + bigEndian(word) + [0,UInt8(count)] + bytes
    }
    public static func selection(_ epc: [UInt8]) throws -> [UInt8] {
        guard (2...62).contains(epc.count), epc.count % 2 == 0 else { throw StripeError("Select a tag with a valid, complete EPC first.") }
        return [0,UInt8(epc.count)] + epc
    }
    public static func parseRead(_ data: [UInt8], expectedEPC: [UInt8], count: Int) throws -> UHFMemoryRead {
        if data.count <= 2 { throw data.first.map(error) ?? StripeError("Empty UHF read response.") }
        guard data.count >= 10, data[0] == 0, data[1] == 1 else { throw StripeError("Read did not identify exactly one matching tag. Keep one tag near the reader.") }
        let section = Int(data[2]), epcLength = Int(data[3] >> 3)*2
        guard epcLength > 0, section == 4+epcLength+count*2,
              (section+6...section+7).contains(data.count), 5+epcLength <= data.count else { throw StripeError("UHF memory response has an unexpected length.") }
        let epc = Array(data[5..<5+epcLength])
        guard epc == expectedEPC else { throw StripeError("The reader returned a different EPC. Nothing more was sent.") }
        let reported = Int(data[section+3]) << 8 | Int(data[section+4])
        guard reported == count*2 else { throw StripeError("The tag returned fewer memory bytes than requested.") }
        return UHFMemoryRead(epc:epc,pc:Array(data[3...4]),bytes:Array(data[(7+epcLength)..<(3+section)]),antenna:Int(data[section+5]))
    }
    public static func checkWrite(_ data: [UInt8], oldEPC: [UInt8], newEPC: [UInt8]?) throws {
        if data.count <= 2 { try acknowledge(data); return }
        guard data.count >= 10, data[0] == 0, data[1] == 1 else { throw StripeError("Write response did not identify one tag. The write may be partial; read the tag before retrying.") }
        let length = Int(data[3] >> 3)*2, section = Int(data[2])
        guard length > 0, section == length+4, (length+9...length+10).contains(data.count) else { throw StripeError("Invalid UHF write result. Read the tag before retrying.") }
        let epc = Array(data[5..<5+length])
        guard epc == oldEPC || epc == newEPC else { throw StripeError("Write response identified a different EPC. Read the tag before retrying.") }
        let status = data[7+length]
        guard status == 0x10 else { throw error(status) }
    }
    public static func epcPayload(_ epc: [UInt8], currentPC: [UInt8]) throws -> [UInt8] {
        _ = try selection(epc)
        guard currentPC.count == 2 else { throw StripeError("Read the tag's protocol-control word before editing its EPC.") }
        return [(currentPC[0] & 7) | UInt8((epc.count/2) << 3),currentPC[1]] + epc
    }
    public static func hex(_ string: String, maxBytes: Int = 128) throws -> [UInt8] {
        let compact = string.filter { !$0.isWhitespace && $0 != ":" && $0 != "-" }
        guard !compact.isEmpty, compact.count % 2 == 0, compact.count/2 <= maxBytes else { throw StripeError("Enter complete hexadecimal bytes (up to \(maxBytes) bytes).") }
        return try HexBytes.parse(compact,count:compact.count/2)
    }
    private static func bigEndian(_ value: UInt32) -> [UInt8] { [24,16,8,0].map { UInt8(truncatingIfNeeded:value >> $0) } }
}

@MainActor public protocol UHFTransport: AnyObject {
    var onTag: ((UHFTag) -> Void)? { get set }
    func exchange(_ command: UInt8, data: [UInt8], timeout: TimeInterval) async throws -> [UInt8]
    func close()
}

@MainActor public final class UHFClient {
    public let transport: any UHFTransport
    public var onInventoryError: ((Error) -> Void)?
    public init(transport: any UHFTransport) { self.transport = transport }
    public func command(_ command: UInt8, _ data: [UInt8] = []) async throws -> [UInt8] { try await transport.exchange(command,data:data,timeout:3) }
    public func select(_ epc: [UInt8]) async throws {
        let filter = try UHFProtocol.selection(epc)
        try UHFProtocol.acknowledge(await command(0x85,filter))
        guard try await command(0x86) == filter else { throw StripeError("The reader did not confirm the selected EPC. No tag write was sent.") }
    }
    public func read(epc: [UInt8], bank: UHFMemoryBank, word: UInt32, count: Int, password: [UInt8]) async throws -> UHFMemoryRead {
        let payload = try UHFProtocol.readCommand(bank:bank,word:word,count:count,password:password)
        try await select(epc)
        let data = try await command(0x81,payload)
        return try UHFProtocol.parseRead(data,expectedEPC:epc,count:count)
    }
    public func write(epc: [UInt8], bank: UHFMemoryBank, word: UInt32, bytes: [UInt8], password: [UInt8], newEPC: [UInt8]? = nil, expectedPC: [UInt8]? = nil) async throws -> UHFMemoryRead {
        let payload = try UHFProtocol.writeCommand(bank:bank,word:word,bytes:bytes,password:password)
        // Re-read the selected tag immediately before sending one write command. No automatic write retries.
        let current = try await read(epc:epc,bank:.epc,word:1,count:1,password:password)
        if let expectedPC, current.bytes != expectedPC { throw StripeError("The tag's control word changed. Read the tag again before changing its EPC.") }
        try Task.checkCancellation()
        let response: [UInt8]
        do { response = try await command(0x82,payload); try UHFProtocol.checkWrite(response,oldEPC:epc,newEPC:newEPC) }
        catch { throw StripeError("Write was sent, but success is not confirmed: \(error.localizedDescription) Read or scan the tag before trying again.") }
        do {
            let result = try await read(epc:newEPC ?? epc,bank:bank,word:word,count:bytes.count/2,password:password)
            guard result.bytes == bytes else { throw StripeError("Read-back data differs from the requested data.") }
            return result
        } catch { throw StripeError("The reader acknowledged the write, but verification failed: \(error.localizedDescription) Do not assume the tag is unchanged.") }
    }
    public func changeEPC(from old: [UInt8], to new: [UInt8], password: [UInt8]) async throws -> UHFMemoryRead {
        _ = try UHFProtocol.selection(new)
        let current = try await read(epc:old,bank:.epc,word:1,count:1,password:password)
        let bytes = try UHFProtocol.epcPayload(new,currentPC:current.bytes)
        return try await write(epc:old,bank:.epc,word:1,bytes:bytes,password:password,newEPC:new,expectedPC:current.bytes)
    }
}
