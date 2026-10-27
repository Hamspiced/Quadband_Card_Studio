// SPDX-License-Identifier: GPL-3.0-only
import Foundation

/// Password-free page-0 programming via Chameleon command 3016.
/// Legacy format writers set block 7 and enable PWD, and are never allowed.
public enum T5577 {
    public static let passwordWriters: Set<UInt16> = [3001,3003,3005,3006,3011,3015,3018,3020]
    public static let formats: Set<LFFormat> = [.em410x,.electra,.viking,.ioprox,.pac,.jablotron]
    public static func validateCommand(_ command: UInt16, data: [UInt8]) throws {
        guard !passwordWriters.contains(command) else { throw StripeError("This command sets a T5577 password and is disabled. Use password-free block writing.") }
        if command == 3016 {
            guard data.count == 11, data[0] < 7, data.suffix(6).allSatisfy({ $0 == 0 }) else {
                throw StripeError("Only open page-0 writes to blocks 0–6 are allowed. Password writes are disabled.")
            }
            if data[0] == 0, data[4] & 0x10 != 0 { throw StripeError("T5577 password protection must remain disabled.") }
        }
    }
    public static func payload(block: Int, word: UInt32) throws -> [UInt8] {
        guard (0..<7).contains(block) else { throw StripeError("The T5577 password block cannot be written.") }
        let data = [UInt8(block)] + [24,16,8,0].map { UInt8(truncatingIfNeeded:word >> $0) } + Array(repeating:UInt8(0),count:6)
        try validateCommand(3016,data:data); return data
    }
    public static func blocks(_ card: LFCard) throws -> [(Int,UInt32)] {
        _ = try LFCard(format:card.format,bytes:card.bytes)
        guard formats.contains(card.format) else { throw StripeError("Password-free writing of this LF format is not implemented. Reading and saving remain available.") }
        var raw: [UInt8] = [], config: UInt32 = 0
        let data = card.bytes
        func pack(_ bits: [UInt8]) -> [UInt8] {
            stride(from:0,to:bits.count,by:8).map { i in bits[i..<i+8].reduce(UInt8(0)) { ($0 << 1) | $1 } }
        }
        switch card.format {
        case .em410x,.electra:
            var bits = Array(repeating:UInt8(1),count:9), columns: UInt8 = 0
            for byte in data.prefix(5) {
                for nibble in [byte >> 4,byte & 15] {
                    bits += [3,2,1,0].map { (nibble >> $0) & 1 }
                    bits.append(UInt8(nibble.nonzeroBitCount % 2)); columns ^= nibble
                }
            }
            bits += [3,2,1,0].map { (columns >> $0) & 1 }; bits.append(0)
            raw = pack(bits)
            if card.format == .electra { raw += data.dropFirst(5) }
            config = 0x00148000 | UInt32((raw.count/4) << 5)
        case .viking:
            raw = [0xF2,0,0] + data + [data.reduce(0x5A,^)]
            config = 0x00088040
        case .ioprox:
            raw = Array(data[4..<12]); config = 0x00147040
        case .jablotron:
            raw = [255,255] + data + [data.reduce(UInt8(0),&+) ^ 0x3A]
            config = 0x00158040
        case .pac:
            let body: [UInt8] = [2,0x32,0x30] + data + [data.reduce(0,^)]
            var bits = Array(repeating:UInt8(1),count:8)
            for byte in body {
                let seven: [UInt8] = (0..<7).map { (byte >> $0) & 1 }
                bits += [0] + seven + [1 - (seven.reduce(0,+) % 2),1]
            }
            raw = pack(bits); config = 0x00080080
        default: throw StripeError("Unsupported password-free LF format.")
        }
        guard config & 0x10 == 0, raw.count <= 24 else { throw StripeError("Invalid password-free configuration.") }
        var words: [(Int,UInt32)] = stride(from:0,to:raw.count,by:4).map { index in
            (index/4+1,raw[index..<index+4].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) })
        }
        words.append((0,config)) // Publish the unprotected configuration after data; never touch block 7.
        return words
    }
}
