// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public enum HexBytes {
    public static func parse(_ text: String, count: Int) throws -> [UInt8] {
        let compact = text.filter { !$0.isWhitespace && $0 != ":" && $0 != "-" }
        guard compact.utf8.count == count * 2, compact.allSatisfy({ $0.isASCII && $0.isHexDigit }) else {
            throw StripeError("Enter exactly \(count) bytes (\(count * 2) hexadecimal digits). Spaces are optional.")
        }
        let chars = Array(compact)
        return stride(from: 0, to: chars.count, by: 2).map { UInt8(String(chars[$0...$0 + 1]), radix: 16)! }
    }
    public static func string(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02X", $0) }.joined(separator: " ") }
    public static func ascii(_ bytes: [UInt8]) -> String {
        String(bytes.map { (32...126).contains($0) ? Character(UnicodeScalar($0)) : "." })
    }
}

public struct NFCCommandError: LocalizedError {
    public let operation: String
    public let status: UInt16
    public var errorDescription: String? {
        let detail: String
        switch status {
        case 0x6300, 0x6982: detail = "Check the key, access permissions and card placement."
        case 0x6A81, 0x6D00, 0x6E00: detail = "This reader or card does not support the command."
        case 0x6985: detail = "The card's access conditions prevent this operation."
        default: detail = "Check the reader, card and selected key."
        }
        return "\(operation) failed (\(String(format: "%04X", status))). \(detail)"
    }
}

public enum ClassicKeyType: String, CaseIterable { case a = "Key A", b = "Key B"
    public var code: UInt8 { self == .a ? 0x60 : 0x61 }
}

public protocol NFCTransport: AnyObject {
    var atr: [UInt8] { get }
    func transmit(_ bytes: [UInt8]) throws -> [UInt8]
    func resetAuthentication() throws
}

public extension NFCTransport { func resetAuthentication() throws {} }

public struct NFCCardInfo: Equatable {
    public var uid: [UInt8]
    public var atr: [UInt8]
    public var atqa: [UInt8]?
    public var sak: UInt8?
    public var isClassic1K: Bool {
        if let sak, let atqa { return sak == 0x08 && atqa.count == 2 && (atqa[0] & 0x1F) == 4 && atqa[1] == 0 && [4,7].contains(uid.count) }
        // PC/SC storage-card ATR: RID A000000306, ISO14443A-3, card name 0001.
        return atr.count == 20 && Array(atr.prefix(15)) == [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1]
    }
    public var name: String { isClassic1K ? "MIFARE Classic 1K" : "Other NFC card" }
    public init(uid: [UInt8], atr: [UInt8], atqa: [UInt8]? = nil, sak: UInt8? = nil) { self.uid = uid; self.atr = atr; self.atqa = atqa; self.sak = sak }
}

public struct ClassicCredential: Equatable {
    public let bytes: [UInt8]
    public let type: ClassicKeyType
    public init(bytes: [UInt8], type: ClassicKeyType) { self.bytes = bytes; self.type = type }
}
public struct ClassicSnapshot {
    public var card: NFCCardInfo
    public var blocks: [Int: [UInt8]]
    public var errors: [Int: String]
    public var sectorKeys: [Int: ClassicCredential] = [:]
    public init(card: NFCCardInfo, blocks: [Int: [UInt8]] = [:], errors: [Int: String] = [:]) {
        self.card = card; self.blocks = blocks; self.errors = errors
    }
}

public final class ClassicClient {
    let transport: any NFCTransport
    public init(transport: any NFCTransport) { self.transport = transport }
    public static func isDataBlock(_ block: Int) -> Bool { (1..<64).contains(block) && block % 4 != 3 }
    func command(_ bytes: [UInt8], operation: String, count: Int? = nil) throws -> [UInt8] {
        let reply = try transport.transmit(bytes)
        guard reply.count >= 2 else { throw StripeError("\(operation): the NFC reader returned an incomplete response.") }
        let sw = UInt16(reply[reply.count - 2]) << 8 | UInt16(reply.last!)
        guard sw == 0x9000 else { throw NFCCommandError(operation: operation, status: sw) }
        let payload = Array(reply.dropLast(2))
        if let count, payload.count != count { throw StripeError("\(operation): expected \(count) bytes, received \(payload.count).") }
        return payload
    }
    public func identify() throws -> NFCCardInfo {
        let uid = try command([0xFF,0xCA,0,0,0], operation: "Read card ID")
        guard [4,7,8,10].contains(uid.count) else { throw StripeError("The NFC reader returned an unsupported card ID length.") }
        return NFCCardInfo(uid: uid, atr: transport.atr)
    }
    func requireClassic(_ card: NFCCardInfo) throws {
        guard card.isClassic1K else { throw StripeError("This card was not identified as MIFARE Classic 1K. Its ID can be read, but this memory editor requires a Classic 1K card.") }
    }
    func loadKey(_ key: [UInt8]) throws {
        guard key.count == 6 else { throw StripeError("A MIFARE Classic key must contain six bytes.") }
        _ = try command([0xFF,0x82,0,0,6] + key, operation: "Load authentication key", count: 0)
    }
    func authenticate(_ block: Int, type: ClassicKeyType) throws {
        guard (0..<64).contains(block) else { throw StripeError("Classic 1K has blocks 0 through 63.") }
        _ = try command([0xFF,0x86,0,0,5,1,0,UInt8(block),type.code,0], operation: "Authenticate sector \(block / 4)", count: 0)
    }
    func readBlock(_ block: Int) throws -> [UInt8] {
        try command([0xFF,0xB0,0,UInt8(block),16], operation: "Read block \(block)", count: 16)
    }
    public func read(key: [UInt8], type: ClassicKeyType, sector: Int? = nil) throws -> ClassicSnapshot {
        let card = try identify(); try requireClassic(card)
        guard sector == nil || (0..<16).contains(sector!) else { throw StripeError("Choose a sector between 0 and 15.") }
        try loadKey(key)
        var snapshot = ClassicSnapshot(card: card)
        for sectorIndex in sector.map({ [$0] }) ?? Array(0..<16) {
            try Task.checkCancellation()
            do { try authenticate(sectorIndex * 4, type: type) }
            catch let error as NFCCommandError {
                for block in sectorIndex * 4..<sectorIndex * 4 + 4 { snapshot.errors[block] = error.localizedDescription }
                try transport.resetAuthentication()
                guard try identify() == card else { throw StripeError("The card changed during the read. Read it again.") }
                try loadKey(key)
                continue
            }
            for block in sectorIndex * 4..<sectorIndex * 4 + 4 {
                try Task.checkCancellation()
                do { snapshot.blocks[block] = try readBlock(block) }
                catch let error as NFCCommandError { snapshot.errors[block] = error.localizedDescription }
            }
        }
        return snapshot
    }
    public func writeProfile(_ profile: CardCredentials, key: [UInt8], type: ClassicKeyType) throws -> NFCCardInfo {
        let data = try profile.validatedBlocks()
        let info = try identify(); try requireClassic(info)
        try loadKey(key)
        // Authenticate and read every target data block before changing any bytes.
        for sector in 0..<16 {
            try Task.checkCancellation()
            try authenticate(sector * 4, type: type)
            for block in sector * 4..<sector * 4 + 3 where Self.isDataBlock(block) {
                _ = try readBlock(block)
            }
        }
        var verified = 0
        do {
            for sector in 0..<16 {
                try Task.checkCancellation()
                try authenticate(sector * 4, type: type)
                for block in sector * 4..<sector * 4 + 3 where Self.isDataBlock(block) {
                    try Task.checkCancellation()
                    let desired = data[block]!
                    if try readBlock(block) != desired {
                        _ = try command([0xFF,0xD6,0,UInt8(block),16] + desired, operation: "Write block \(block)", count: 0)
                    }
                    guard try readBlock(block) == desired else { throw StripeError("Block \(block) did not match on read-back.") }
                    verified += 1
                }
            }
        } catch {
            throw StripeError("Copy incomplete after verifying \(verified) of 47 data blocks. The card may be partially changed. \(error.localizedDescription)")
        }
        return info
    }

    public func write(block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType,
                      expectedCard: NFCCardInfo, previous: [UInt8]) throws -> [UInt8] {
        guard Self.isDataBlock(block) else { throw StripeError("Choose a data block. Block 0 and sector trailers are read-only in this editor.") }
        guard data.count == 16, previous.count == 16 else { throw StripeError("A Classic block must contain exactly 16 bytes.") }
        let actual = try identify(); try requireClassic(actual)
        guard actual == expectedCard else { throw StripeError("The card changed. Read this card before writing.") }
        try loadKey(key); try authenticate(block, type: type)
        guard try readBlock(block) == previous else { throw StripeError("This block changed since it was read. Read the card again before writing.") }
        try Task.checkCancellation()
        do {
            _ = try command([0xFF,0xD6,0,UInt8(block),16] + data, operation: "Write block \(block)", count: 0)
            let result = try readBlock(block)
            guard result == data else { throw StripeError("Read-back data differs from the requested data.") }
            return result
        } catch {
            throw StripeError("The write was attempted but could not be verified. Read the card again. \(error.localizedDescription)")
        }
    }
}
