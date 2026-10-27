// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

/// Native Chameleon reader commands. Never uses emulation-memory or Gen1a backdoor commands.
@MainActor public final class ChameleonHFClient {
    private let client: ChameleonClient
    public init(client: ChameleonClient) { self.client = client }
    private func require(_ command: UInt16) throws {
        guard client.info?.capabilities.contains(command) == true else { throw StripeError("The connected Chameleon firmware does not support this HF operation.") }
    }
    public static func decodeCard(_ bytes: [UInt8]) throws -> NFCCardInfo {
        guard let count = bytes.first, [4,7,10].contains(count), bytes.count >= Int(count)+5 else { throw StripeError("Invalid Chameleon HF card response.") }
        let n = Int(count), atsLength = Int(bytes[n+4])
        guard bytes.count == n+5+atsLength else { throw StripeError("Keep exactly one HF card on the Chameleon antenna.") }
        return NFCCardInfo(uid:Array(bytes[1...n]),atr:[],atqa:Array(bytes[n+1...n+2]),sak:bytes[n+3])
    }
    public func identify() async throws -> NFCCardInfo {
        try require(2000); try await client.prepareReader()
        let reply = try await client.command(2000)
        if reply.status == 1 { throw StripeError("Place one HF card on the Chameleon antenna.") }
        guard reply.status == 0 else { throw Self.error(reply.status) }
        return try Self.decodeCard(reply.data)
    }
    public func isPresent() async throws -> Bool {
        try require(2000); try await client.prepareReader()
        let reply = try await client.command(2000)
        if reply.status == 1 { return false }
        guard reply.status == 0 else { throw Self.error(reply.status) }
        _ = try Self.decodeCard(reply.data); return true
    }
    private func requireClassic(_ card: NFCCardInfo) throws {
        guard card.isClassic1K else { throw StripeError("This HF memory workflow supports MIFARE Classic 1K, including Gen1a in normal authenticated mode. This card can be identified but cannot be written here.") }
    }
    private func sameCard(_ expected: NFCCardInfo) async throws {
        guard try await identify() == expected else { throw StripeError("The HF card changed. Read the target again before writing.") }
    }
    private func readBlock(_ block: Int, credential: ClassicCredential) async throws -> [UInt8] {
        try require(2008)
        guard (0..<64).contains(block), credential.bytes.count == 6 else { throw StripeError("Invalid Classic block or key.") }
        let reply = try await client.command(2008,[credential.type.code,UInt8(block)]+credential.bytes)
        guard reply.status == 0 else { throw Self.error(reply.status) }
        guard reply.data.count == 16 else { throw StripeError("Incomplete Classic block returned by Chameleon.") }
        return reply.data
    }
    private func writeBlock(_ block: Int, bytes: [UInt8], credential: ClassicCredential, expected: NFCCardInfo) async throws {
        try require(2009)
        guard ClassicClient.isDataBlock(block), bytes.count == 16, credential.bytes.count == 6 else { throw StripeError("Only 16-byte Classic data blocks may be written. UID and sector trailers are protected.") }
        try await sameCard(expected)
        let reply = try await client.command(2009,[credential.type.code,UInt8(block)]+credential.bytes+bytes)
        guard reply.status == 0, reply.data.isEmpty else { throw Self.error(reply.status) }
        guard try await readBlock(block,credential:credential) == bytes else { throw StripeError("HF block \(block) did not match on read-back.") }
        try await sameCard(expected)
    }
    public func read(key: [UInt8], type: ClassicKeyType, sector: Int? = nil, automatic: Bool = true) async throws -> ClassicSnapshot {
        guard key.count == 6, sector == nil || (0..<16).contains(sector!) else { throw StripeError("Choose a valid Classic sector and six-byte key.") }
        let card = try await identify(); try requireClassic(card); try require(2008)
        let supplied = ClassicCredential(bytes:key,type:type), factory = ClassicCredential(bytes:Array(repeating:255,count:6),type:.a)
        var result = ClassicSnapshot(card:card)
        for sector in sector.map({ [$0] }) ?? Array(0..<16) {
            try Task.checkCancellation()
            let standard = ClassicCredential(bytes:sector == 0 ? [0xA0,0xA1,0xA2,0xA3,0xA4,0xA5] : [0xD3,0xF7,0xD3,0xF7,0xD3,0xF7],type:.a)
            let candidates = automatic ? (supplied == factory ? [standard,factory] : [supplied,standard,factory]) : [supplied]
            var chosen: ClassicCredential?, failure = "No configured key could read this sector."
            for credential in candidates {
                do { result.blocks[sector*4] = try await readBlock(sector*4,credential:credential); chosen = credential; break }
                catch let error as NFCCommandError { failure = error.localizedDescription }
            }
            guard let chosen else { for block in sector*4..<sector*4+4 { result.errors[block] = failure }; continue }
            result.sectorKeys[sector] = chosen
            for block in sector*4+1..<sector*4+4 {
                do { result.blocks[block] = try await readBlock(block,credential:chosen) }
                catch let error as NFCCommandError { result.errors[block] = error.localizedDescription }
            }
        }
        try await sameCard(card); return result
    }
    public func write(block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) async throws -> [UInt8] {
        try require(2009); try requireClassic(expected)
        guard ClassicClient.isDataBlock(block), data.count == 16, previous.count == 16, key.count == 6 else { throw StripeError("Choose a data block and valid 16-byte data. UID and sector trailers cannot be written.") }
        let credential = ClassicCredential(bytes:key,type:type)
        try await sameCard(expected)
        guard try await readBlock(block,credential:credential) == previous else { throw StripeError("This HF block changed since it was read. Read it again before writing.") }
        do { try await writeBlock(block,bytes:data,credential:credential,expected:expected); return data }
        catch { throw StripeError("HF write was attempted but could not be verified. Read the card again. \(error.localizedDescription)") }
    }
    public func writeProfile(_ profile: CardCredentials, key: [UInt8], type: ClassicKeyType) async throws -> NFCCardInfo {
        try require(2009)
        let blocks = try profile.validatedBlocks()
        let snapshot = try await read(key:key,type:type,automatic:true)
        guard snapshot.errors.isEmpty, snapshot.blocks.count == 64 else { throw StripeError("Read all target sectors before copying. Check the keys and card placement.") }
        var verified = 0
        do {
            for block in blocks.keys.sorted() {
                guard let credential = snapshot.sectorKeys[block/4], let bytes = blocks[block] else { throw StripeError("Missing target key or source data.") }
                if snapshot.blocks[block] != bytes { try await writeBlock(block,bytes:bytes,credential:credential,expected:snapshot.card) }
                else { guard try await readBlock(block,credential:credential) == bytes else { throw StripeError("The card changed during copying.") } }
                verified += 1
            }
            try await sameCard(snapshot.card); return snapshot.card
        } catch { throw StripeError("Copy incomplete after verifying \(verified) of 47 data blocks. The target may be partially changed. \(error.localizedDescription)") }
    }
    public func writeNDEF(_ records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) async throws -> ClassicSnapshot {
        try require(2009)
        let snapshot = try await read(key:key,type:type)
        let current = try ClassicNDEF(snapshot:snapshot)
        guard current.card == expected.card, current.memory == expected.memory, current.blocks == expected.blocks else { throw StripeError("The card or NFC records changed. Read it again before writing.") }
        guard current.writable else { throw StripeError("The NFC area is read-only with the available key.") }
        let final = try current.replacing(with:records)
        var staging = final; staging[current.lengthPosition] = 0
        let commit = current.lengthPosition/16
        func update(_ index: Int, bytes: [UInt8]) async throws {
            let block = current.blocks[index]
            guard let credential = snapshot.sectorKeys[block/4] else { throw StripeError("The NFC sector key is unavailable.") }
            try await writeBlock(block,bytes:bytes,credential:credential,expected:snapshot.card)
        }
        do {
            var initial = Array(current.memory[commit*16..<commit*16+16]); initial[current.lengthPosition%16] = 0
            try await update(commit,bytes:initial)
            for index in 0..<current.blocks.count {
                let bytes = Array(staging[index*16..<index*16+16])
                if index == commit || bytes != Array(current.memory[index*16..<index*16+16]) { try await update(index,bytes:bytes) }
            }
            try await update(commit,bytes:Array(final[commit*16..<commit*16+16]))
            let result = try await read(key:key,type:type)
            guard result.card == snapshot.card, try ClassicNDEF(snapshot:result).records == records else { throw StripeError("NFC records did not match on final verification.") }
            return result
        } catch { throw StripeError("Record write did not finish verification. The NFC message may be empty or incomplete; read the card again. \(error.localizedDescription)") }
    }
    private static func error(_ status: UInt16) -> Error {
        switch status {
        case 1: return StripeError("The HF card was removed. Place it on the Chameleon antenna and read it again.")
        case 6: return NFCCommandError(operation:"Authenticate Classic card",status:0x6300)
        case 2,3,4,5,7: return NFCCommandError(operation:"Read/write Classic card",status:0x6300)
        default: return ChameleonClient.error(status)
        }
    }
}
