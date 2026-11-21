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
    public struct Geometry: Codable, Equatable {
        public let unitSize: Int
        public let unitCount: Int
        public init(unitSize: Int, unitCount: Int) { self.unitSize = unitSize; self.unitCount = unitCount }
    }
    public var family: CardFamily?
    public var geometry: Geometry?
    public var cardFamily: CardFamily { family ?? .classic1k }
    public var name: String
    public var createdAt: Date
    public var blocks: [Block]
    public init(snapshot: ClassicSnapshot, name: String? = nil) throws {
        let kind = snapshot.card.family
        guard kind != .unknown else { throw StripeError("Identify a supported card before saving its data.") }
        if kind != .classic1k {
            format = "quadband.card.data"; version = 2; family = kind
            geometry = Geometry(unitSize:kind.unitSize,unitCount:kind.unitCount)
        }
        self.name = name ?? (kind == .classic1k ? "Classic 1K data" : "\(kind.rawValue) data"); createdAt = Date()
        blocks = kind.dataUnits.compactMap { block in
            snapshot.blocks[block].map { Block(number: block, hex: HexBytes.string($0)) }
        }
        _ = try validatedBlocks()
    }
    public func validatedBlocks() throws -> [Int: [UInt8]] {
        let legacy = format == "quadband.classic1k.data" && version == 1 && family == nil && geometry == nil
        let modern = format == "quadband.card.data" && version == 2 && family != nil && cardFamily != .unknown &&
            geometry == Geometry(unitSize:cardFamily.unitSize,unitCount:cardFamily.unitCount)
        guard legacy || modern else {
            throw StripeError("This is not a supported Quadband Classic 1K credentials file.")
        }
        guard !name.isEmpty, name.count <= 100, !name.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else {
            throw StripeError("The credentials file has an invalid profile name.")
        }
        let expected = Set(cardFamily.dataUnits)
        guard blocks.count == expected.count, Set(blocks.map(\.number)) == expected else {
            throw StripeError("A data profile requires all \(expected.count) eligible units exactly once. Protected metadata cannot be included.")
        }
        var result: [Int: [UInt8]] = [:]
        for block in blocks { result[block.number] = try HexBytes.parse(block.hex, count: cardFamily.unitSize) }
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
