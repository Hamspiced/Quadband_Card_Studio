import Foundation

public enum CardFamily: String, Codable, CaseIterable {
    case classic1k, classic4k, ntag213, ntag215, ntag216, unknown
    public var isClassic: Bool { self == .classic1k || self == .classic4k }
    public var isNTAG: Bool { [.ntag213,.ntag215,.ntag216].contains(self) }
    public var unitSize: Int { isClassic ? 16 : 4 }
    public var unitCount: Int {
        switch self { case .classic1k: return 64; case .classic4k: return 256; case .ntag213: return 45
        case .ntag215: return 135; case .ntag216: return 231; case .unknown: return 0 }
    }
    public var sectorCount: Int { self == .classic4k ? 40 : isClassic ? 16 : 0 }
    public var dynamicLockPage: Int { unitCount - 5 }
    public var userPages: Range<Int> { isNTAG ? 4..<dynamicLockPage : 0..<0 }
    public func firstBlock(_ sector: Int) -> Int { sector < 32 ? sector * 4 : 128 + (sector - 32) * 16 }
    public func blocks(in sector: Int) -> Range<Int> {
        guard (0..<sectorCount).contains(sector) else { return 0..<0 }
        let first = firstBlock(sector)
        return first..<first + (sector < 32 ? 4 : 16)
    }
    public func sector(of block: Int) -> Int { block < 128 ? block / 4 : 32 + (block - 128) / 16 }
    public func isTrailer(_ block: Int) -> Bool {
        isClassic && (0..<unitCount).contains(block) && block == blocks(in:sector(of:block)).last
    }
    public func accessGroup(_ block: Int) -> Int {
        let sector = sector(of:block), offset = block - firstBlock(sector)
        return isTrailer(block) ? 3 : sector < 32 ? offset : offset / 5
    }
    public var dataUnits: [Int] {
        isClassic ? (1..<unitCount).filter { !isTrailer($0) } : Array(userPages)
    }
    public func isData(_ unit: Int) -> Bool { isClassic ? unit > 0 && unit < unitCount && !isTrailer(unit) : userPages.contains(unit) }
}

public enum ClassicAccess {
    public static func writable(block: Int, family: CardFamily, trailer: [UInt8], key: ClassicKeyType) throws -> Bool {
        guard family.isData(block), trailer.count == 16 else { return false }
        let index = family.accessGroup(block)
        for group in 0..<4 {
            let c1 = (trailer[7] >> (4 + group)) & 1, c2 = (trailer[8] >> group) & 1, c3 = (trailer[8] >> (4 + group)) & 1
            guard c1 ^ ((trailer[6] >> group) & 1) == 1,
                  c2 ^ ((trailer[6] >> (4 + group)) & 1) == 1,
                  c3 ^ ((trailer[7] >> group) & 1) == 1 else { throw StripeError("Inconsistent Classic access bits. Writing is disabled.") }
        }
        let c1 = Int((trailer[7] >> (4 + index)) & 1)
        let c2 = Int((trailer[8] >> index) & 1)
        let c3 = Int((trailer[8] >> (4 + index)) & 1)
        let access = c1 * 4 + c2 * 2 + c3
        return access == 0 || ([3,4,6].contains(access) && key == .b)
    }
}

public enum NTAGAuthentication: Equatable {
    case none
    case password([UInt8])
    case passwordAndPACK([UInt8], [UInt8])
    public func encoded(supported: Bool) throws -> [UInt8] {
        switch self {
        case .none: return [0]
        case .password(let bytes):
            guard supported, bytes.count == 4 else { throw StripeError("NTAG password authentication is unavailable or invalid.") }; return [1] + bytes
        case .passwordAndPACK(let bytes, let pack):
            guard supported, bytes.count == 4, pack.count == 2 else { throw StripeError("NTAG password/PACK authentication is unavailable or invalid.") }; return [2] + bytes + pack
        }
    }
}

public struct NTAGPermissions {
    public let family: CardFamily
    public let pages: [Int:[UInt8]]
    public init(family: CardFamily, pages: [Int:[UInt8]]) { self.family = family; self.pages = pages }
    public func requireWritable(_ page: Int, authenticated: Bool) throws {
        guard family.isNTAG, family.userPages.contains(page),
              let staticLock = pages[2], staticLock.count == 4,
              let cc = pages[3], cc.count == 4, cc[0] == 0xE1, cc[1] >> 4 == 1, cc[3] == 0,
              Int(cc[2]) * 8 > 0, Int(cc[2]) * 8 <= family.userPages.count * 4,
              let dynamic = pages[family.dynamicLockPage], dynamic.count == 4,
              let config = pages[family.dynamicLockPage + 1], config.count == 4,
              let access = pages[family.dynamicLockPage + 2], access.count == 4 else {
            throw StripeError("NTAG permissions are unknown, protected or read-only. Read CC, locks and configuration first.")
        }
        if page <= 15 {
            let locks = UInt16(staticLock[2]) | UInt16(staticLock[3]) << 8
            guard locks & (1 << page) == 0 else { throw StripeError("This NTAG page is statically locked.") }
        } else {
            let groupSize = family == .ntag213 ? 2 : 16
            let bit = (page - 16) / groupSize
            let locks = UInt32(dynamic[0]) | UInt32(dynamic[1]) << 8 | UInt32(dynamic[2]) << 16
            guard locks & (1 << bit) == 0 else { throw StripeError("This NTAG page is dynamically locked.") }
        }
        guard config[3] == 0xFF || page < Int(config[3]) || authenticated else { throw StripeError("This NTAG page requires an explicitly supplied password.") }
        guard access[0] & 0x07 == 0 || authenticated || page < Int(config[3]) else { throw StripeError("NTAG authentication is required.") }
    }
}
