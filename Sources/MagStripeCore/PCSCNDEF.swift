import Foundation

extension ClassicClient {
    func readNTAGPages(_ page: Int, count: Int, card: NFCCardInfo) throws -> [UInt8] {
        guard card.family.isNTAG, (1...4).contains(count), page >= 0, page + count <= card.family.unitCount - 2 else { throw StripeError("NTAG read exceeds readable geometry.") }
        let start = min(page,card.family.unitCount-6)
        let bytes = try command([255,0xB0,0,UInt8(start),16],operation:"Read NTAG pages",count:16)
        let offset = (page-start)*4
        return Array(bytes[offset..<offset+count*4])
    }
    func readNTAG(_ card: NFCCardInfo) throws -> ClassicSnapshot {
        var snapshot = ClassicSnapshot(card:card)
        for page in stride(from:0,to:card.family.unitCount-2,by:4) {
            try Task.checkCancellation()
            let count = min(4,card.family.unitCount-2-page), bytes = try readNTAGPages(page,count:min(4,card.family.unitCount-2-page),card:card)
            for offset in 0..<count { snapshot.blocks[page+offset] = Array(bytes[offset*4..<offset*4+4]) }
        }
        guard try identify() == card else { throw StripeError("Card changed during Type 2 read.") }
        return snapshot
    }
    func writeNTAG(page: Int, data: [UInt8], expected: NFCCardInfo, previous: [UInt8]) throws -> [UInt8] {
        guard data.count == 4, previous.count == 4, expected.family.isData(page), try identify() == expected else { throw StripeError("Invalid or changed Type 2 target.") }
        var metadata: [Int:[UInt8]] = [:]
        for unit in [2,3,expected.family.dynamicLockPage,expected.family.dynamicLockPage+1,expected.family.dynamicLockPage+2] {
            metadata[unit] = try readNTAGPages(unit,count:1,card:expected)
        }
        try NTAGPermissions(family:expected.family,pages:metadata).requireWritable(page,authenticated:false)
        guard try readNTAGPages(page,count:1,card:expected) == previous else { throw StripeError("NTAG page changed since review.") }
        try Task.checkCancellation()
        do {
            _ = try command([255,0xD6,0,UInt8(page),4]+data,operation:"Write NTAG user page",count:0)
            let verified = try readNTAGPages(page,count:1,card:expected)
            guard verified == data, try identify() == expected else { throw StripeError("Type 2 verification mismatch.") }
            return verified
        } catch { throw StripeError("Type 2 write attempted; outcome unverified. \(error.localizedDescription)") }
    }
    public func formatNDEF(expected: ClassicSnapshot, overwrite: Bool = false) throws -> ClassicSnapshot {
        let reviewed = try NDEFFormatting.snapshotBytes(expected,overwrite:overwrite)
        let family = expected.card.family
        var current = try read(key:NDEFFormatting.factory,type:.a)
        guard current.card == expected.card,
              try NDEFFormatting.snapshotBytes(current,overwrite:overwrite) == reviewed else { throw StripeError("Formatting target changed since review.") }
        var attempted = false
        do {
            for (unit,desired) in try NDEFFormatting.canonicalUnits(family) {
                try Task.checkCancellation()
                guard try identify() == expected.card else { throw StripeError("Formatting identity changed.") }
                if family.isClassic {
                    try loadKey(NDEFFormatting.factory); try authenticate(unit,type:.a)
                    let trailerUnit = family.blocks(in:family.sector(of:unit)).last!
                    let permissions = try readBlock(trailerUnit)
                    guard Array(permissions[6...]) == Array(current.blocks[trailerUnit]![6...]) else { throw StripeError("Factory permissions changed during formatting.") }
                    let actual = try readBlock(unit), previous = current.blocks[unit]!
                    guard family.isTrailer(unit) ? Array(actual[6...]) == Array(previous[6...]) : actual == previous else { throw StripeError("Formatting bytes changed.") }
                } else {
                    for metadata in [2,3,family.dynamicLockPage,family.dynamicLockPage+1,family.dynamicLockPage+2] {
                        guard try readNTAGPages(metadata,count:1,card:expected.card) == current.blocks[metadata] else { throw StripeError("NTAG permissions changed during initialization.") }
                    }
                    guard try readNTAGPages(unit,count:1,card:expected.card) == current.blocks[unit] else { throw StripeError("Initialization bytes changed.") }
                }
                attempted = true
                _ = try command([255,0xD6,0,UInt8(unit),UInt8(desired.count)]+desired,operation:"Canonical NDEF format",count:0)
                if family.isClassic {
                    if family.isTrailer(unit) {
                        try transport.resetAuthentication()
                        guard try identify() == expected.card else { throw StripeError("Card changed after sector transition.") }
                        try loadKey(Array(desired.prefix(6))); try authenticate(unit,type:.a)
                    }
                    let actual = try readBlock(unit)
                    guard family.isTrailer(unit) ? Array(actual[6..<10]) == Array(desired[6..<10]) : actual == desired else { throw StripeError("Canonical format read-back mismatch.") }
                } else {
                    guard try readNTAGPages(unit,count:1,card:expected.card) == desired else { throw StripeError("CC initialization read-back mismatch.") }
                }
                current.blocks[unit] = desired
            }
            let result = try readAutomatic(key:NDEFFormatting.factory,type:.a)
            guard result.card == expected.card, try ClassicNDEF(snapshot:result).records.isEmpty else { throw StripeError("Canonical format final verification failed.") }
            return result
        } catch {
            if attempted { throw StripeError("Formatting attempted; target unverified. Do not retry automatically. \(error.localizedDescription)") }
            throw error
        }
    }
}
