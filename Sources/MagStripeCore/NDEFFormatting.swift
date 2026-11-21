import Foundation

public enum NDEFFormatting {
    public static let factory = Array(repeating: UInt8(255), count: 6)
    public static let dataTrailer: [UInt8] = [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7,0x7F,0x07,0x88,0x40,255,255,255,255,255,255]
    public static let directoryTrailer: [UInt8] = [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0x78,0x77,0x88,0xC1,255,255,255,255,255,255]
    public static func cc(_ family: CardFamily) throws -> [UInt8] {
        guard family.isNTAG else { throw StripeError("Canonical CC requires an exact NTAG family.") }
        return [0xE1,0x10,family == .ntag213 ? 0x12 : family == .ntag215 ? 0x3E : 0x6D,0]
    }
    public static func versionFamily(_ version: [UInt8]) -> CardFamily {
        guard version.count == 8, Array(version.prefix(6)) == [0,4,4,2,1,0], version[7] == 3 else { return .unknown }
        switch version[6] { case 0x0F: return .ntag213; case 0x11: return .ntag215; case 0x13: return .ntag216; default: return .unknown }
    }
    public static func snapshotBytes(_ snapshot: ClassicSnapshot, overwrite: Bool) throws -> [UInt8] {
        let family = snapshot.card.family
        guard family == .classic1k || family.isNTAG, snapshot.errors.isEmpty else { throw StripeError("Formatting requires a complete readable supported target.") }
        let count = family == .classic1k ? 64 : family.unitCount - 2
        var bytes: [UInt8] = []
        for unit in 0..<count {
            guard let value = snapshot.blocks[unit], value.count == family.unitSize else { throw StripeError("Formatting requires every readable snapshot unit.") }
            bytes += value
        }
        if family == .classic1k {
            guard ![UInt8(0xC1),0xC2].contains(snapshot.blocks[3]![9]) else {
                throw StripeError("Existing or partial MAD formatting cannot be repaired.")
            }
            for sector in 0..<16 {
                guard snapshot.sectorKeys[sector] == ClassicCredential(bytes:factory,type:.a),
                      let trailer = snapshot.blocks[sector*4+3],
                      Array(trailer[6..<9]) == [255,7,128], Array(trailer[10..<16]) == factory else {
                    throw StripeError("Only proven factory-key, factory-access Classic1K cards can be formatted.")
                }
            }
            guard overwrite || family.dataUnits.allSatisfy({ snapshot.blocks[$0]!.allSatisfy { $0 == 0 } }) else { throw StripeError("Existing data requires explicit overwrite authorization.") }
        } else {
            guard versionFamily(snapshot.card.version) == family, snapshot.blocks[3] == [0,0,0,0],
                  snapshot.blocks[2]![2] == 0, snapshot.blocks[2]![3] == 0,
                  snapshot.blocks[family.dynamicLockPage]!.prefix(3).allSatisfy({ $0 == 0 }),
                  snapshot.blocks[family.dynamicLockPage+1]![3] == 255,
                  !snapshot.ntagAuthenticated else { throw StripeError("Only exact, unlocked, unprotected NTAG parts with an all-zero CC can be initialized.") }
            guard overwrite || family.userPages.allSatisfy({ snapshot.blocks[$0]!.allSatisfy { $0 == 0 } }) else { throw StripeError("Existing data requires explicit overwrite authorization.") }
        }
        return bytes
    }
    public static func canonicalUnits(_ family: CardFamily) throws -> [(Int,[UInt8])] {
        if family == .classic1k {
            var result: [(Int,[UInt8])] = []
            for (index,block) in (4..<64).filter({ $0 % 4 != 3 }).enumerated() {
                result.append((block,index == 0 ? [3,0,254] + Array(repeating:0,count:13) : Array(repeating:0,count:16)))
            }
            for sector in 1..<16 { result.append((sector*4+3,dataTrailer)) }
            var mad: [UInt8] = [0,1] + Array(repeating:[UInt8(3),0xE1],count:15).flatMap { $0 }
            mad[0] = ClassicNDEF.crc(Array(mad.dropFirst()))
            result += [(1,Array(mad.prefix(16))),(2,Array(mad.suffix(16))),(3,directoryTrailer)]
            return result
        }
        let container = try cc(family), pages = Int(container[2]) * 2
        var result = (4..<4+pages).map { ($0,$0 == 4 ? [UInt8(3),0,254,0] : Array(repeating:UInt8(0),count:4)) }
        result.append((3,container))
        return result
    }
}
