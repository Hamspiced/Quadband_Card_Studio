// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

/// A portable data-only profile. It deliberately contains no UID, keys or trailers.
public struct CardCredentials: Codable, Equatable {
    public struct Block: Codable, Equatable {
        public var number: Int
        public var hex: String
        public init(number: Int, hex: String) { self.number = number; self.hex = hex }
    }
    public var format = "quadband.classic1k.data"
    public var version = 1
    public var name: String
    public var createdAt: Date
    public var blocks: [Block]
    public init(snapshot: ClassicSnapshot, name: String = "Classic 1K data") throws {
        guard snapshot.card.isClassic1K else { throw StripeError("Scan a Classic 1K card before saving its data.") }
        self.name = name; createdAt = Date()
        blocks = (0..<64).filter(ClassicClient.isDataBlock).compactMap { block in
            snapshot.blocks[block].map { Block(number: block, hex: HexBytes.string($0)) }
        }
        _ = try validatedBlocks()
    }
    public func validatedBlocks() throws -> [Int: [UInt8]] {
        guard format == "quadband.classic1k.data", version == 1 else {
            throw StripeError("This is not a supported Quadband Classic 1K credentials file.")
        }
        guard !name.isEmpty, name.count <= 100, !name.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else {
            throw StripeError("The credentials file has an invalid profile name.")
        }
        let expected = Set((0..<64).filter(ClassicClient.isDataBlock))
        guard blocks.count == 47, Set(blocks.map(\.number)) == expected else {
            throw StripeError("A data profile requires all 47 data blocks exactly once. Read all sectors with their correct keys. UID and sector-trailer blocks cannot be included.")
        }
        var result: [Int: [UInt8]] = [:]
        for block in blocks { result[block.number] = try HexBytes.parse(block.hex, count: 16) }
        return result
    }
    public func encoded() throws -> Data {
        _ = try validatedBlocks()
        let encoder = JSONEncoder(); encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(self)
    }
    public static func decode(_ data: Data) throws -> CardCredentials {
        guard data.count <= 65536 else { throw StripeError("This credentials file is too large. Choose a Quadband .qcard file.") }
        let decoder = JSONDecoder(); decoder.dateDecodingStrategy = .iso8601
        let result: CardCredentials
        do { result = try decoder.decode(Self.self, from: data) }
        catch { throw StripeError("This file is not a valid Quadband credentials file. Save a scan as .qcard, then open that file.") }
        _ = try result.validatedBlocks()
        return result
    }
}
