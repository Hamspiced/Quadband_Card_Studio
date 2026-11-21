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
    var readerName: String { get }
    func transmit(_ bytes: [UInt8]) throws -> [UInt8]
    func resetAuthentication() throws
}

public extension NFCTransport {
    var readerName: String { "" }
    func resetAuthentication() throws {}
}

public struct NFCCardInfo: Equatable {
    public var uid: [UInt8]
    public var atr: [UInt8]
    public var atqa: [UInt8]?
    public var sak: UInt8?
    public var detectedFamily: CardFamily?
    public var version: [UInt8] = []
    public var family: CardFamily {
        if let detectedFamily { return detectedFamily }
        if isClassic1K { return .classic1k }
        if let sak, let atqa, sak == 0x18, atqa.count == 2, atqa[0] & 0x1F == 2, atqa[1] == 0, [4,7].contains(uid.count) { return .classic4k }
        if atr.count == 20, Array(atr.prefix(14)) == [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0], atr[14] == 2 { return .classic4k }
        return .unknown
    }
    public var isClassic1K: Bool {
        if let sak, let atqa { return sak == 0x08 && atqa.count == 2 && (atqa[0] & 0x1F) == 4 && atqa[1] == 0 && [4,7].contains(uid.count) }
        // PC/SC storage-card ATR: RID A000000306, ISO14443A-3, card name 0001.
        return atr.count == 20 && Array(atr.prefix(15)) == [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1]
    }
    public var name: String {
        switch family { case .classic1k: return "MIFARE Classic 1K"; case .classic4k: return "MIFARE Classic 4K"
        case .ntag213: return "NTAG213"; case .ntag215: return "NTAG215"; case .ntag216: return "NTAG216"; case .unknown: return "Other NFC card" }
    }
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
    public var ntagAuthenticated = false
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
        var card = NFCCardInfo(uid: uid, atr: transport.atr)
        if card.family == .unknown, card.atr.count == 20,
           Array(card.atr.prefix(14)) == [0x3B,0x8F,0x80,1,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0],
           card.atr[14] == 3 {
            let wrapped: [UInt8] = [255,0,0,0,4,0xD4,0x40,1,0x60], raw: [UInt8] = [255,0,0,0,1,0x60]
            let commands = transport.readerName.uppercased().contains("ACR1552") ? [raw,wrapped] : [wrapped,raw]
            var version: [UInt8]?
            for (index,apdu) in commands.enumerated() {
                try Task.checkCancellation()
                do {
                    let reply = try command(apdu,operation:"NTAG GET_VERSION")
                    let bytes = reply.count == 11 && Array(reply.prefix(3)) == [0xD5,0x41,0] ? Array(reply.dropFirst(3)) : reply
                    guard bytes.count == 8 else { throw StripeError("Reader did not return an exact NTAG version.") }
                    version = bytes; break
                } catch let error as NFCCommandError where index == 0 && [UInt16(0x6A81),0x6D00,0x6E00,0x6700].contains(error.status) {
                    try transport.resetAuthentication()
                    guard try command([255,0xCA,0,0,0],operation:"Recheck Type 2 ID") == uid else { throw StripeError("Type 2 target changed during version detection.") }
                }
            }
            guard let version else { throw StripeError("Exact NTAG version unavailable.") }
            card.version = version
            let family = NDEFFormatting.versionFamily(card.version)
            guard family.isNTAG else { throw StripeError("Unsupported Type 2 version. Memory access disabled.") }
            card.detectedFamily = family
        }
        return card
    }
    func requireClassic(_ card: NFCCardInfo) throws {
        guard card.family.isClassic else { throw StripeError("This card was not identified as MIFARE Classic 1K or 4K. Its ID is readable; memory writing is disabled.") }
    }
    func loadKey(_ key: [UInt8]) throws {
        guard key.count == 6 else { throw StripeError("A MIFARE Classic key must contain six bytes.") }
        _ = try command([0xFF,0x82,0,0,6] + key, operation: "Load authentication key", count: 0)
    }
    func authenticate(_ block: Int, type: ClassicKeyType) throws {
        guard (0..<256).contains(block) else { throw StripeError("Classic has blocks 0 through 255; the identified family limits operations.") }
        _ = try command([0xFF,0x86,0,0,5,1,0,UInt8(block),type.code,0], operation: "Authenticate sector \(block / 4)", count: 0)
    }
    func readBlock(_ block: Int) throws -> [UInt8] {
        try command([0xFF,0xB0,0,UInt8(block),16], operation: "Read block \(block)", count: 16)
    }
    public func read(key: [UInt8], type: ClassicKeyType, sector: Int? = nil) throws -> ClassicSnapshot {
        let card = try identify()
        if card.family.isNTAG { return try readNTAG(card) }
        try requireClassic(card)
        guard sector == nil || (0..<card.family.sectorCount).contains(sector!) else { throw StripeError("Choose a sector between 0 and 15.") }
        try loadKey(key)
        var snapshot = ClassicSnapshot(card: card)
        for sectorIndex in sector.map({ [$0] }) ?? Array(0..<card.family.sectorCount) {
            try Task.checkCancellation()
            do { try authenticate(card.family.firstBlock(sectorIndex), type: type) }
            catch let error as NFCCommandError {
                for block in card.family.blocks(in:sectorIndex) { snapshot.errors[block] = error.localizedDescription }
                try transport.resetAuthentication()
                guard try identify() == card else { throw StripeError("The card changed during the read. Read it again.") }
                try loadKey(key)
                continue
            }
            snapshot.sectorKeys[sectorIndex] = ClassicCredential(bytes:key,type:type)
            for block in card.family.blocks(in:sectorIndex) {
                try Task.checkCancellation()
                do { snapshot.blocks[block] = try readBlock(block) }
                catch let error as NFCCommandError { snapshot.errors[block] = error.localizedDescription }
            }
        }
        try Task.checkCancellation()
        guard try identify() == card else { throw StripeError("The card changed during the read. Read it again.") }
        return snapshot
    }
    public func writeProfile(_ profile: CardCredentials, key: [UInt8], type: ClassicKeyType) throws -> NFCCardInfo {
        let data = try profile.validatedBlocks()
        let snapshot = try readAutomatic(key:key,type:type), info = snapshot.card
        guard profile.cardFamily == info.family else { throw StripeError("Profile family does not match the target card.") }
        for block in data.keys {
            let sector = info.family.sector(of:block)
            guard let credential = snapshot.sectorKeys[sector], snapshot.blocks[block]?.count == 16,
                  let trailer = snapshot.blocks[info.family.blocks(in:sector).last!],
                  try ClassicAccess.writable(block:block,family:info.family,trailer:trailer,key:credential.type) else {
                throw StripeError("Read every target data block with writable access permissions before copying.")
            }
        }
        var verified = 0
        do {
            for block in data.keys.sorted() {
                try Task.checkCancellation()
                let desired = data[block]!, previous = snapshot.blocks[block]!, credential = snapshot.sectorKeys[info.family.sector(of:block)]!
                if previous != desired {
                    _ = try write(block:block,data:desired,key:credential.bytes,type:credential.type,expectedCard:info,previous:previous)
                } else {
                    guard try identify() == info else { throw StripeError("Card changed during copy.") }
                    try loadKey(credential.bytes); try authenticate(block,type:credential.type)
                    guard try readBlock(block) == desired else { throw StripeError("Target data changed after preflight.") }
                }
                verified += 1
            }
            guard try identify() == info else { throw StripeError("Card changed during final verification.") }
        } catch {
            throw StripeError("Copy incomplete after verifying \(verified) of \(data.count) data blocks. The card may be partially changed. \(error.localizedDescription)")
        }
        return info
    }

    public func write(block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType,
                      expectedCard: NFCCardInfo, previous: [UInt8]) throws -> [UInt8] {
        if expectedCard.family.isNTAG { return try writeNTAG(page:block,data:data,expected:expectedCard,previous:previous) }
        guard expectedCard.family.isData(block) else { throw StripeError("Choose a data block. Block 0 and sector trailers are read-only in this editor.") }
        guard data.count == 16, previous.count == 16 else { throw StripeError("A Classic block must contain exactly 16 bytes.") }
        let actual = try identify(); try requireClassic(actual)
        guard actual == expectedCard else { throw StripeError("The card changed. Read this card before writing.") }
        try loadKey(key); try authenticate(block, type: type)
        let trailer = try readBlock(actual.family.blocks(in:actual.family.sector(of:block)).last!)
        guard try ClassicAccess.writable(block:block,family:actual.family,trailer:trailer,key:type) else { throw StripeError("Card access bits prohibit writing this block.") }
        guard try readBlock(block) == previous else { throw StripeError("This block changed since it was read. Read the card again before writing.") }
        guard try identify() == expectedCard else { throw StripeError("The card changed before the write. Read it again.") }
        try Task.checkCancellation()
        do {
            _ = try command([0xFF,0xD6,0,UInt8(block),16] + data, operation: "Write block \(block)", count: 0)
            let result = try readBlock(block)
            guard result == data else { throw StripeError("Read-back data differs from the requested data.") }
            guard try identify() == expectedCard else { throw StripeError("The card changed during write verification.") }
            return result
        } catch {
            throw StripeError("The write was attempted but could not be verified. Read the card again. \(error.localizedDescription)")
        }
    }
}
