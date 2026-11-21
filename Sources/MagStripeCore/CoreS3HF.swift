import Foundation

@MainActor public final class CoreS3HFClient {
    private let session: CoreS3Session
    private let lease: CoreS3Session.Lease
    private let authentication: NTAGAuthentication
    private var slot: CoreS3Capabilities.Peripheral? { session.capabilities?.peripherals.first { $0.channel == 1 && $0.generation == lease.generation } }
    public init(session: CoreS3Session, lease: CoreS3Session.Lease, authentication: NTAGAuthentication = .none) {
        self.session = session; self.lease = lease; self.authentication = authentication
    }
    public func formatNDEF(expected: ClassicSnapshot, overwrite: Bool = false) async throws -> ClassicSnapshot {
        let family = expected.card.family
        try require(family == .classic1k ? "classic1k_format" : "ntag_initialize")
        guard authentication == .none, slot?.kind == "rfid" || slot?.kind == "rfid2" else { throw StripeError("Formatting requires an identified module without supplied NTAG authentication.") }
        let bytes = try NDEFFormatting.snapshotBytes(expected,overwrite:overwrite)
        guard try await identify() == expected.card else { throw StripeError("Formatting target changed.") }
        let payload = try prefix(expected.card) + [overwrite ? 1 : 0] + bytes
        let reply = try await session.perform(lease,operation:family == .classic1k ? 22 : 23,payload:payload,write:true,timeout:120)
        guard reply.isEmpty else { throw StripeError("Formatting response invalid; outcome unverified.") }
        do {
            let result = try await read(key:NDEFFormatting.factory,type:.a)
            guard result.card == expected.card, try ClassicNDEF(snapshot:result).records.isEmpty else { throw StripeError("Canonical formatting final verification mismatch.") }
            return result
        } catch { throw StripeError("Formatting attempted; outcome unverified. \(error.localizedDescription)") }
    }
    private func require(_ capability: String) throws {
        guard slot?.present == true, slot?.capabilities.contains(capability) == true else { throw StripeError("The selected CoreS3 HF module does not advertise \(capability).") }
    }
    private func prefix(_ card: NFCCardInfo) throws -> [UInt8] {
        guard [4,7,10].contains(card.uid.count) else { throw StripeError("Invalid bridge card UID.") }
        return [UInt8(card.uid.count)] + card.uid
    }
    private func auth() throws -> [UInt8] { try authentication.encoded(supported:slot?.capabilities.contains("ntag_auth") == true) }
    public func identify() async throws -> NFCCardInfo {
        try require("identify")
        struct Identity: Decodable {
            let uid: String; let atqa: String; let sak: UInt8; let family: CardFamily
            let blockCount: Int; let pageCount: Int; let version: String
        }
        let bytes = try await session.perform(lease,operation:16)
        let value = try JSONDecoder().decode(Identity.self,from:Data(bytes))
        guard [8,14,20].contains(value.uid.count),
              value.blockCount == (value.family.isClassic ? value.family.unitCount : 0),
              value.pageCount == (value.family.isNTAG ? value.family.unitCount : 0) else { throw StripeError("Invalid CoreS3 card geometry.") }
        var info = NFCCardInfo(uid:try HexBytes.parse(value.uid,count:value.uid.count/2),atr:[],
                              atqa:try HexBytes.parse(value.atqa,count:2),sak:value.sak)
        if value.family.isNTAG {
            let version = try HexBytes.parse(value.version,count:8)
            let storage: UInt8 = value.family == .ntag213 ? 0x0F : value.family == .ntag215 ? 0x11 : 0x13
            guard Array(version.prefix(6)) == [0,4,4,2,1,0], version[6] == storage, version[7] == 3, value.sak == 0 else { throw StripeError("NTAG version does not match its claimed family.") }
            info.version = version
        } else if value.family.isClassic {
            guard info.family == value.family else { throw StripeError("Classic RF identity does not match its claimed geometry.") }
        }
        info.detectedFamily = value.family; return info
    }
    public func isPresent() async throws -> Bool {
        do { _ = try await identify(); return true } catch let error as CoreS3Error where error.status == 10 { return false }
    }
    private func readBlock(_ block: Int, card: NFCCardInfo, credential: ClassicCredential) async throws -> [UInt8] {
        try require(card.family.rawValue)
        guard card.family.isClassic, (0..<card.family.unitCount).contains(block), credential.bytes.count == 6 else { throw StripeError("Invalid Classic read.") }
        let payload = try prefix(card) + UInt16(block).bridgeLE + [credential.type.code] + credential.bytes
        let result = try await session.perform(lease,operation:17,payload:payload)
        guard result.count == 16 else { throw StripeError("Incomplete Classic bridge response.") }; return result
    }
    public func readPages(_ page: Int, count: Int, card: NFCCardInfo) async throws -> [UInt8] {
        try require("ntag21x")
        guard card.family.isNTAG, (1...16).contains(count), page >= 0, page <= card.family.unitCount - count else { throw StripeError("NTAG read exceeds the identified geometry.") }
        let payload = try prefix(card) + UInt16(page).bridgeLE + [UInt8(count)] + auth()
        let result = try await session.perform(lease,operation:19,payload:payload)
        guard result.count == count * 4 else { throw StripeError("Incomplete NTAG page response.") }; return result
    }
    public func read(key: [UInt8], type: ClassicKeyType, sector: Int? = nil, automatic: Bool = true) async throws -> ClassicSnapshot {
        let card = try await identify(), family = card.family
        var result = ClassicSnapshot(card:card)
        if family.isNTAG {
            // Password/PACK pages are not fetched or represented as known credentials.
            for page in stride(from:0,to:family.dynamicLockPage + 3,by:16) {
                try Task.checkCancellation()
                let count = min(16,family.dynamicLockPage + 3 - page)
                let bytes = try await readPages(page,count:count,card:card)
                for offset in 0..<count { result.blocks[page+offset] = Array(bytes[offset*4..<offset*4+4]) }
            }
            result.ntagAuthenticated = authentication != .none
        } else {
            guard family.isClassic, key.count == 6, sector == nil || (0..<family.sectorCount).contains(sector!) else { throw StripeError("Choose a supported Classic family, sector and six-byte key.") }
            let supplied = ClassicCredential(bytes:key,type:type), factory = ClassicCredential(bytes:Array(repeating:255,count:6),type:.a)
            for sector in sector.map({ [$0] }) ?? Array(0..<family.sectorCount) {
                try Task.checkCancellation()
                let standard = ClassicCredential(bytes:(sector == 0 || family == .classic4k && sector == 16) ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7],type:.a)
                let candidates = automatic ? (supplied == factory ? [standard,factory] : [supplied,standard,factory]) : [supplied]
                var chosen: ClassicCredential?, failure = "No supplied or standard key could read this sector."
                for credential in candidates {
                    do {
                        result.blocks[family.firstBlock(sector)] = try await readBlock(family.firstBlock(sector),card:card,credential:credential)
                        chosen = credential; break
                    } catch let error as CoreS3Error where error.status == 11 {
                        failure = error.localizedDescription
                        _ = try await session.perform(lease,operation:21)
                    }
                }
                guard let chosen else { for block in family.blocks(in:sector) { result.errors[block] = failure }; continue }
                result.sectorKeys[sector] = chosen
                for block in family.blocks(in:sector).dropFirst() {
                    do { result.blocks[block] = try await readBlock(block,card:card,credential:chosen) }
                    catch let error as CoreS3Error where error.status == 11 || error.status == 16 { result.errors[block] = error.localizedDescription }
                }
            }
        }
        guard try await identify() == card else { throw StripeError("Card changed during the read.") }
        return result
    }
    public func write(block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) async throws -> [UInt8] {
        let family = expected.family
        guard family.isData(block), data.count == family.unitSize, previous.count == family.unitSize,
              slot?.kind == "rfid" || slot?.kind == "rfid2",
              try await identify() == expected else { throw StripeError("Card, geometry or target unit changed. Read the target again.") }
        let payload: [UInt8], operation: UInt8
        if family.isNTAG {
            try require("ntag21x")
            var pages: [Int:[UInt8]] = [:]
            for (start,count) in [(2,2),(family.dynamicLockPage,3)] {
                let bytes = try await readPages(start,count:count,card:expected)
                for offset in 0..<count { pages[start+offset] = Array(bytes[offset*4..<offset*4+4]) }
            }
            try NTAGPermissions(family:family,pages:pages).requireWritable(block,authenticated:authentication != .none)
            payload = try prefix(expected) + UInt16(block).bridgeLE + auth() + previous + data
            operation = 20
        } else {
            try require(family.rawValue)
            guard key.count == 6, slot?.kind == "rfid" || slot?.kind == "rfid2" else { throw StripeError("Unknown HF module or invalid Classic key.") }
            let credential = ClassicCredential(bytes:key,type:type)
            let trailer = try await readBlock(family.blocks(in:family.sector(of:block)).last!,card:expected,credential:credential)
            guard try ClassicAccess.writable(block:block,family:family,trailer:trailer,key:type) else { throw StripeError("Classic access bits prohibit this write.") }
            payload = try prefix(expected) + UInt16(block).bridgeLE + [type.code] + key + previous + data
            operation = 18
        }
        let verified = try await session.perform(lease,operation:operation,payload:payload,write:true)
        guard verified == data else { throw StripeError("Write was attempted but the bridge read-back did not match. Outcome unverified.") }
        return verified
    }
    private func preflight(_ snapshot: ClassicSnapshot, units: [Int]) throws {
        let family = snapshot.card.family
        for unit in units {
            guard snapshot.blocks[unit]?.count == family.unitSize else { throw StripeError("Read every target unit before writing.") }
            if family.isNTAG {
                try NTAGPermissions(family:family,pages:snapshot.blocks).requireWritable(unit,authenticated:snapshot.ntagAuthenticated)
            } else {
                let sector = family.sector(of:unit)
                guard let credential = snapshot.sectorKeys[sector], let trailer = snapshot.blocks[family.blocks(in:sector).last!],
                      try ClassicAccess.writable(block:unit,family:family,trailer:trailer,key:credential.type) else { throw StripeError("Target access permissions prohibit this operation.") }
            }
        }
    }
    public func writeProfile(_ profile: CardCredentials, key: [UInt8], type: ClassicKeyType) async throws -> NFCCardInfo {
        let data = try profile.validatedBlocks(), snapshot = try await read(key:key,type:type)
        guard profile.cardFamily == snapshot.card.family else { throw StripeError("The saved profile family does not match the target.") }
        try preflight(snapshot,units:data.keys.sorted())
        var verified = 0
        do {
            for unit in data.keys.sorted() {
                try Task.checkCancellation()
                let credential = snapshot.sectorKeys[snapshot.card.family.sector(of:unit)]
                _ = try await write(block:unit,data:data[unit]!,key:credential?.bytes ?? key,type:credential?.type ?? type,
                                    expected:snapshot.card,previous:snapshot.blocks[unit]!)
                verified += 1
            }
            return snapshot.card
        } catch { throw StripeError("Copy incomplete after \(verified) of \(data.count) units. Target may be partially changed. \(error.localizedDescription)") }
    }
    public func writeNDEF(_ records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) async throws -> ClassicSnapshot {
        var snapshot = try await read(key:key,type:type)
        let current = try ClassicNDEF(snapshot:snapshot)
        guard current.card == expected.card, current.memory == expected.memory, current.blocks == expected.blocks, current.writable else { throw StripeError("The NFC area changed or is not writable. Read it again.") }
        try preflight(snapshot,units:current.blocks)
        let final = try current.replacing(with:records), size = current.unitSize, position = current.lengthPosition
        var staging = final; staging[position] = 0
        let commit = position / size
        func update(_ index: Int, _ bytes: [UInt8]) async throws {
            let unit = current.blocks[index], credential = snapshot.sectorKeys[snapshot.card.family.sector(of:unit)]
            let verified = try await write(block:unit,data:bytes,key:credential?.bytes ?? key,type:credential?.type ?? type,
                                           expected:snapshot.card,previous:snapshot.blocks[unit]!)
            snapshot.blocks[unit] = verified
        }
        do {
            var initial = Array(current.memory[commit*size..<commit*size+size]); initial[position % size] = 0
            try await update(commit,initial)
            for index in 0..<current.blocks.count {
                try Task.checkCancellation()
                let bytes = Array(staging[index*size..<index*size+size])
                if snapshot.blocks[current.blocks[index]] != bytes { try await update(index,bytes) }
            }
            try await update(commit,Array(final[commit*size..<commit*size+size]))
            let result = try await read(key:key,type:type)
            guard try ClassicNDEF(snapshot:result).records == records else { throw StripeError("NDEF final read-back mismatch.") }
            return result
        } catch { throw StripeError("NDEF write incomplete or unverified. Message may be empty; read the target again. \(error.localizedDescription)") }
    }
}
