// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct ClassicNDEF {
    public let card: NFCCardInfo
    public let blocks: [Int]
    public let memory: [UInt8]
    public let records: [NDEFRecord]
    public let messageSize: Int
    public let writable: Bool
    let prefix: [UInt8]
    let suffix: [UInt8]
    public var capacity: Int { max(0, memory.count - prefix.count - suffix.count - (memory.count > 256 ? 4 : 2)) }
    public init(snapshot: ClassicSnapshot) throws {
        guard snapshot.card.isClassic1K, let one = snapshot.blocks[1], let two = snapshot.blocks[2], let madTrailer = snapshot.blocks[3], madTrailer.count == 16 else { throw StripeError("The card directory could not be read. Try automatic standard keys, or enter the card's key in Advanced.") }
        let mad = one + two
        guard mad.count == 32, madTrailer[9] & 0x80 != 0, madTrailer[9] & 3 == 1,
              Self.crc(Array(mad.dropFirst())) == mad[0] else { throw StripeError("This card is not formatted for NFC records. Its data blocks are available under Advanced.") }
        let sectors = (1..<16).filter { mad[$0 * 2] == 0x03 && mad[$0 * 2 + 1] == 0xE1 }
        guard !sectors.isEmpty else { throw StripeError("The card directory does not contain an NFC record area.") }
        blocks = sectors.flatMap { [$0 * 4, $0 * 4 + 1, $0 * 4 + 2] }
        var bytes: [UInt8] = [], canWrite = true
        for block in blocks {
            guard let value = snapshot.blocks[block], value.count == 16 else { throw StripeError("NFC record block \(block) could not be read. Check the selected key in Advanced.") }
            bytes += value
            guard let trailer = snapshot.blocks[block / 4 * 4 + 3], trailer.count == 16 else { throw StripeError("The NFC sector permissions could not be read.") }
            let index = block % 4
            let c1 = (trailer[7] >> (4 + index)) & 1, c2 = (trailer[8] >> index) & 1, c3 = (trailer[8] >> (4 + index)) & 1
            guard c1 ^ ((trailer[6] >> index) & 1) == 1, c2 ^ ((trailer[6] >> (4 + index)) & 1) == 1, c3 ^ ((trailer[7] >> index) & 1) == 1 else { throw StripeError("The card has inconsistent access bits; record writing is disabled.") }
            let access = Int(c1 * 4 + c2 * 2 + c3)
            canWrite = canWrite && (access == 0 || ([3,4,6].contains(access) && snapshot.sectorKeys[block / 4]?.type == .b)) && trailer[9] & 3 == 0
        }
        var cursor = 0, first: (Int,Int,Int)?, lastTLVEnd = 0
        while cursor < bytes.count {
            let start = cursor, tag = bytes[cursor]; cursor += 1
            if tag == 0 { continue }
            if tag == 0xFE { break }
            guard cursor < bytes.count else { throw StripeError("The NFC TLV header is incomplete.") }
            var length = Int(bytes[cursor]); cursor += 1
            if length == 255 {
                guard cursor + 2 <= bytes.count else { throw StripeError("The NFC TLV length is incomplete.") }
                length = Int(bytes[cursor]) * 256 + Int(bytes[cursor + 1]); cursor += 2
            }
            guard length <= bytes.count - cursor else { throw StripeError("The NFC record extends beyond the card's memory.") }
            if tag == 3 && first == nil { first = (start,cursor,cursor + length) }
            cursor += length; lastTLVEnd = cursor
        }
        guard let (start, payloadStart, end) = first else { throw StripeError("The card's NFC area has no NDEF message.") }
        // Ignore unused NULL padding after the message, while preserving other TLVs.
        let tailEnd = lastTLVEnd
        prefix = Array(bytes[..<start]); suffix = Array(bytes[end..<max(end,tailEnd)])
        let message = Array(bytes[payloadStart..<end])
        records = try NDEFCodec.decode(message); messageSize = message.count
        memory = bytes; card = snapshot.card; writable = canWrite
    }
    public static func crc(_ bytes: [UInt8]) -> UInt8 {
        var crc: UInt8 = 0xC7
        for byte in bytes {
            crc ^= byte
            for _ in 0..<8 { crc = crc & 0x80 != 0 ? (crc &<< 1) ^ 0x1D : crc &<< 1 }
        }
        return crc
    }
    public func replacing(with records: [NDEFRecord]) throws -> [UInt8] {
        let message = try NDEFCodec.encode(records)
        guard message.count <= 65535 else { throw StripeError("The NFC message is too large for this card.") }
        let length: [UInt8] = message.count < 255 ? [UInt8(message.count)] : [255,UInt8(message.count >> 8),UInt8(message.count & 255)]
        var bytes = prefix + [3] + length + message + suffix
        guard bytes.count <= memory.count else { throw StripeError("These records use \(message.count) bytes; this card has room for \(capacity). Remove or shorten a record.") }
        if bytes.count < memory.count { bytes.append(0xFE) }
        bytes += Array(repeating: 0, count: memory.count - bytes.count)
        return bytes
    }
    var lengthPosition: Int { prefix.count + 1 }
}

extension ClassicClient {
    public func readAutomatic(key: [UInt8], type: ClassicKeyType, sector: Int? = nil) throws -> ClassicSnapshot {
        let card = try identify(); try requireClassic(card)
        guard key.count == 6, sector == nil || (0..<16).contains(sector!) else { throw StripeError("Choose a valid six-byte key and sector.") }
        let factory = ClassicCredential(bytes: Array(repeating: 0xFF, count: 6), type: .a)
        let supplied = ClassicCredential(bytes: key, type: type)
        var snapshot = ClassicSnapshot(card: card)
        for current in sector.map({ [$0] }) ?? Array(0..<16) {
            try Task.checkCancellation()
            let standard = ClassicCredential(bytes: current == 0 ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7], type: .a)
            let candidates = supplied == factory ? [standard, factory] : [supplied,standard,factory]
            var accepted = false, lastError = "No configured key could read this sector."
            for credential in candidates {
                try loadKey(credential.bytes)
                do {
                    try authenticate(current * 4, type: credential.type)
                    snapshot.sectorKeys[current] = credential; accepted = true; break
                } catch let error as NFCCommandError {
                    lastError = error.localizedDescription
                    // A failed Classic authentication can leave the PICC halted.
                    try transport.resetAuthentication()
                    guard try identify() == card else { throw StripeError("The card changed during the read. Read it again.") }
                }
            }
            for block in current * 4..<current * 4 + 4 {
                if !accepted { snapshot.errors[block] = lastError; continue }
                do { snapshot.blocks[block] = try readBlock(block) }
                catch let error as NFCCommandError { snapshot.errors[block] = error.localizedDescription }
            }
        }
        return snapshot
    }
    public func writeNDEF(_ records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) throws -> ClassicSnapshot {
        let snapshot = try readAutomatic(key: key, type: type)
        let current = try ClassicNDEF(snapshot: snapshot)
        guard current.card == expected.card, current.memory == expected.memory else { throw StripeError("The card or its records changed. Read the target card again before writing.") }
        guard current.writable else { throw StripeError("The NFC area is read-only with the available key.") }
        let final = try current.replacing(with: records)
        // Publish an empty NDEF length before updating payload, and the real length last.
        var staging = final
        staging[current.lengthPosition] = 0
        let commitBlock = current.lengthPosition / 16
        func update(_ index: Int, bytes: [UInt8]) throws {
            let block = current.blocks[index]
            guard let credential = snapshot.sectorKeys[block / 4] else { throw StripeError("The NFC sector key is unavailable.") }
            try loadKey(credential.bytes); try authenticate(block, type: credential.type)
            _ = try command([0xFF,0xD6,0,UInt8(block),16] + bytes, operation: "Write NFC records", count: 0)
            guard try readBlock(block) == bytes else { throw StripeError("NFC record write did not match on read-back.") }
        }
        do {
            try Task.checkCancellation()
            // Invalidate using the original block, so no new payload is visible before length zero.
            var initial = Array(current.memory[commitBlock * 16..<commitBlock * 16 + 16])
            initial[current.lengthPosition % 16] = 0
            try update(commitBlock, bytes: initial)
            for index in 0..<current.blocks.count {
                try Task.checkCancellation()
                let bytes = Array(staging[index * 16..<index * 16 + 16])
                if index == commitBlock || bytes != Array(current.memory[index * 16..<index * 16 + 16]) { try update(index, bytes: bytes) }
            }
            try update(commitBlock, bytes: Array(final[commitBlock * 16..<commitBlock * 16 + 16]))
            let result = try readAutomatic(key: key, type: type)
            guard try ClassicNDEF(snapshot: result).records == records else { throw StripeError("The records did not match on final verification.") }
            return result
        } catch { throw StripeError("Record write did not finish verification. The NFC message may be empty or incomplete; read the card again. \(error.localizedDescription)") }
    }
}
