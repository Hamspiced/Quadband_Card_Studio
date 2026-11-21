// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct ClassicNDEF {
    public let card: NFCCardInfo
    public let blocks: [Int]
    public let memory: [UInt8]
    public let records: [NDEFRecord]
    public let messageSize: Int
    public let writable: Bool
    public var unitSize: Int { card.family.unitSize }
    let prefix: [UInt8]
    let suffix: [UInt8]
    public var capacity: Int { max(0, memory.count - prefix.count - suffix.count - (memory.count > 256 ? 4 : 2)) }
    public init(snapshot: ClassicSnapshot) throws {
        let family = snapshot.card.family
        var bytes: [UInt8] = [], canWrite = true
        if family.isNTAG {
            guard let cc = snapshot.blocks[3], cc.count == 4, cc[0] == 0xE1, cc[1] >> 4 == 1, cc[3] >> 4 == 0 else { throw StripeError("Invalid or unreadable NTAG capability container.") }
            let capacity = Int(cc[2]) * 8
            guard capacity > 0, capacity <= family.userPages.count * 4 else { throw StripeError("NTAG CC capacity exceeds physical user memory.") }
            blocks = Array(family.userPages.prefix((capacity + 3) / 4))
            for page in blocks {
                guard let value = snapshot.blocks[page], value.count == 4 else { throw StripeError("NFC user page \(page) could not be read.") }
                bytes += value
                if (try? NTAGPermissions(family:family,pages:snapshot.blocks).requireWritable(page,authenticated:snapshot.ntagAuthenticated)) == nil { canWrite = false }
            }
            bytes = Array(bytes.prefix(capacity))
        } else {
            guard family.isClassic, let one = snapshot.blocks[1], let two = snapshot.blocks[2], let madTrailer = snapshot.blocks[3], madTrailer.count == 16 else { throw StripeError("The card directory could not be read. Try automatic standard keys, or enter the card's key in Advanced.") }
            let mad = one + two
            let madVersion = madTrailer[9] & 3
            guard mad.count == 32, madTrailer[9] & 0x80 != 0, (madVersion == 1 || family == .classic4k && madVersion == 2),
                  Self.crc(Array(mad.dropFirst())) == mad[0] else { throw StripeError("This card is not formatted for NFC records. Its data blocks are available under Advanced.") }
            _ = try ClassicAccess.writable(block:1,family:family,trailer:madTrailer,key:snapshot.sectorKeys[0]?.type ?? .a)
            var sectors = (1..<16).filter { mad[$0 * 2] == 0x03 && mad[$0 * 2 + 1] == 0xE1 }
            if madVersion == 2 {
                let second = (64...66).flatMap { snapshot.blocks[$0] ?? [] }
                guard second.count == 48, let trailer = snapshot.blocks[67], trailer.count == 16,
                      trailer[9] == 0, Self.crc(Array(second.dropFirst())) == second[0] else { throw StripeError("MAD2 directory is missing or has invalid CRC/GPB.") }
                _ = try ClassicAccess.writable(block:64,family:family,trailer:trailer,key:snapshot.sectorKeys[16]?.type ?? .a)
                sectors += (17..<40).filter { second[2 + ($0 - 17) * 2] == 3 && second[3 + ($0 - 17) * 2] == 0xE1 }
            }
            guard !sectors.isEmpty else { throw StripeError("The card directory does not contain an NFC record area.") }
            blocks = sectors.flatMap { Array(family.blocks(in:$0).dropLast()) }
            for block in blocks {
                guard let value = snapshot.blocks[block], value.count == 16 else { throw StripeError("NFC record block \(block) could not be read. Check the selected key in Advanced.") }
                bytes += value
                let sector = family.sector(of:block)
                guard let trailer = snapshot.blocks[family.blocks(in:sector).last!], trailer.count == 16 else { throw StripeError("The NFC sector permissions could not be read.") }
                canWrite = try canWrite && ClassicAccess.writable(block:block,family:family,trailer:trailer,key:snapshot.sectorKeys[sector]?.type ?? .a) && trailer[9] & 3 == 0
            }
        }
        if family.isNTAG && bytes.allSatisfy({ $0 == 0 }) {
            prefix = []; suffix = []; records = []; messageSize = 0
            memory = bytes; card = snapshot.card; writable = canWrite
            return
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
            if family.isNTAG && [UInt8(1),2].contains(tag) {
                guard length == 3 else { throw StripeError("Invalid Type 2 control TLV.") }
                let descriptor = Array(bytes[cursor..<cursor+3]), exponent = Int(bytes[cursor+2] & 15)
                guard exponent <= 10 else { throw StripeError("Unknown Type 2 reserved-memory addressing.") }
                let address = Int(descriptor[0] >> 4) * (1 << exponent) + Int(descriptor[0] & 15)
                let size = descriptor[1] == 0 ? 256 : Int(descriptor[1])
                let reservedBytes = tag == 1 ? (size + 7) / 8 : size
                let knownLockBits = family == .ntag213 ? 12 : family == .ntag215 ? 8 : 14
                let knownLockExponent = family == .ntag213 ? 3 : 6
                guard address >= 16 + bytes.count, address + reservedBytes <= family.unitCount * 4,
                      Int(descriptor[0] & 15) < (1 << exponent),
                      tag != 1 || (address == family.dynamicLockPage * 4 && size == knownLockBits && descriptor[2] >> 4 == knownLockExponent),
                      tag != 2 || descriptor[2] >> 4 == 0 else {
                    throw StripeError("Type 2 reserved-memory mapping overlaps the NFC area or has an unknown lock layout. Writing is disabled.")
                }
            }
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
    public var lengthPosition: Int { prefix.count + 1 }
}

extension ClassicClient {
    public func readAutomatic(key: [UInt8], type: ClassicKeyType, sector: Int? = nil) throws -> ClassicSnapshot {
        let card = try identify()
        if card.family.isNTAG { return try readNTAG(card) }
        try requireClassic(card)
        guard key.count == 6, sector == nil || (0..<card.family.sectorCount).contains(sector!) else { throw StripeError("Choose a valid six-byte key and sector.") }
        let factory = ClassicCredential(bytes: Array(repeating: 0xFF, count: 6), type: .a)
        let supplied = ClassicCredential(bytes: key, type: type)
        var snapshot = ClassicSnapshot(card: card)
        for current in sector.map({ [$0] }) ?? Array(0..<card.family.sectorCount) {
            try Task.checkCancellation()
            let standard = ClassicCredential(bytes: (current == 0 || card.family == .classic4k && current == 16) ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7], type: .a)
            let candidates = supplied == factory ? [standard, factory] : [supplied,standard,factory]
            var accepted = false, lastError = "No configured key could read this sector."
            for credential in candidates {
                try loadKey(credential.bytes)
                do {
                    try authenticate(card.family.firstBlock(current), type: credential.type)
                    snapshot.sectorKeys[current] = credential; accepted = true; break
                } catch let error as NFCCommandError {
                    lastError = error.localizedDescription
                    // A failed Classic authentication can leave the PICC halted.
                    try transport.resetAuthentication()
                    guard try identify() == card else { throw StripeError("The card changed during the read. Read it again.") }
                }
            }
            for block in card.family.blocks(in:current) {
                if !accepted { snapshot.errors[block] = lastError; continue }
                do { snapshot.blocks[block] = try readBlock(block) }
                catch let error as NFCCommandError { snapshot.errors[block] = error.localizedDescription }
            }
        }
        try Task.checkCancellation()
        guard try identify() == card else { throw StripeError("The card changed during the read. Read it again.") }
        return snapshot
    }
    public func writeNDEF(_ records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) throws -> ClassicSnapshot {
        var snapshot = try readAutomatic(key: key, type: type)
        let current = try ClassicNDEF(snapshot: snapshot)
        guard current.card == expected.card, current.memory == expected.memory else { throw StripeError("The card or its records changed. Read the target card again before writing.") }
        guard current.writable else { throw StripeError("The NFC area is read-only with the available key.") }
        let final = try current.replacing(with: records)
        // Publish an empty NDEF length before updating payload, and the real length last.
        var staging = final
        staging[current.lengthPosition] = 0
        let size = current.unitSize
        let commitBlock = current.lengthPosition / size
        func update(_ index: Int, bytes: [UInt8]) throws {
            let block = current.blocks[index]
            let credential = snapshot.sectorKeys[snapshot.card.family.sector(of:block)]
            snapshot.blocks[block] = try write(block:block,data:bytes,key:credential?.bytes ?? key,type:credential?.type ?? type,expectedCard:snapshot.card,previous:snapshot.blocks[block]!)
        }
        do {
            try Task.checkCancellation()
            // Invalidate using the original block, so no new payload is visible before length zero.
            var initial = Array(current.memory[commitBlock * size..<commitBlock * size + size])
            initial[current.lengthPosition % size] = 0
            try update(commitBlock, bytes: initial)
            for index in 0..<current.blocks.count {
                try Task.checkCancellation()
                let bytes = Array(staging[index * size..<index * size + size])
                if snapshot.blocks[current.blocks[index]] != bytes { try update(index, bytes: bytes) }
            }
            try update(commitBlock, bytes: Array(final[commitBlock * size..<commitBlock * size + size]))
            let result = try readAutomatic(key: key, type: type)
            guard try ClassicNDEF(snapshot: result).records == records else { throw StripeError("The records did not match on final verification.") }
            return result
        } catch { throw StripeError("Record write did not finish verification. The NFC message may be empty or incomplete; read the card again. \(error.localizedDescription)") }
    }
}
