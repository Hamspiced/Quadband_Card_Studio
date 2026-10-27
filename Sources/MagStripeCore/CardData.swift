// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct StripeError: LocalizedError, Equatable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

public struct CardData: Equatable, Codable {
    public var tracks: [String]
    public init(_ tracks: [String] = ["", "", ""]) {
        self.tracks = Array((tracks + ["", "", ""]).prefix(3))
    }
    public static let shortTest = CardData(["MIDWEST TEST 12345", "1234567890=12345", "9876543210=54321"])
    public static let originalTest = CardData([
        "B4111111111111111^GADGETS/MIDWEST^2912101000000000000000",
        "4111111111111111=29121010000000000000",
        "011234567890=4111111111111111=840=0000=0000=0000000000000000"
    ])
    public var isEmpty: Bool { tracks.allSatisfy(\.isEmpty) }
    public static func payload(_ text: String, track: Int) -> String {
        // Preserve spaces in the payload: they are legal Track 1 data.
        var value = text.trimmingCharacters(in: .newlines)
        if (track == 0 && value.first == "%") || (track > 0 && value.first == ";") || (track == 2 && value.first == "+") {
            value.removeFirst()
        }
        if value.last == "?" { value.removeLast() }
        return value
    }
    public var normalized: CardData {
        CardData(tracks.enumerated().map { Self.payload($0.element, track: $0.offset) })
    }
    public func framed(_ track: Int) -> String {
        let value = normalized.tracks[track]
        return value.isEmpty ? "" : (track == 0 ? "%" : ";") + value + "?"
    }
    public func validate(densities: [Int]) throws {
        guard tracks.count == 3, densities.count == 3 else { throw StripeError("Three tracks are required.") }
        let values = normalized.tracks
        guard !values.allSatisfy(\.isEmpty) else { throw StripeError("Enter data for at least one track.") }
        for i in 0..<3 {
            guard [75, 210].contains(densities[i]) else { throw StripeError("Choose 75 or 210 BPI.") }
            let bytes = Array(values[i].utf8)
            let valid = bytes.allSatisfy { byte in
                if i == 0 { return (32...95).contains(byte) && byte != 0x3F && byte != 0x25 }
                return (48...62).contains(byte) && byte != 0x3B
            }
            guard valid else {
                throw StripeError("Track \(i + 1): \(i == 0 ? "use uppercase ASCII letters, numbers and punctuation" : "use digits and : < = >"). Sentinels belong only at the ends.")
            }
            let limit = Self.payloadLimit(track: i, density: densities[i])
            guard bytes.count <= limit else { throw StripeError("Track \(i + 1) has \(bytes.count) characters; the limit at \(densities[i]) BPI is \(limit). Shorten the data or select 210 BPI.") }
        }
    }
    public static func payloadLimit(track: Int, density: Int) -> Int {
        // Conservative ISO-sized recording area, reserving SS, ES and LRC.
        density == 75 ? (track == 0 ? 25 : 37) : (track == 0 ? 76 : 104)
    }
}

public struct EncodingSettings: Equatable {
    public var densities: [Int]
    public var highCoercivity: Bool
    public init(densities: [Int] = [210, 75, 210], highCoercivity: Bool = true) {
        self.densities = densities
        self.highCoercivity = highCoercivity
    }
}

public enum SwipeInputFormat: String, CaseIterable {
    case automatic = "Automatic · track markers"
    case tracks123 = "Three fields → Tracks 1, 2, 3"
    case tracks12 = "Two fields → Tracks 1, 2"
    case tracks23 = "Two fields → Tracks 2, 3"
    case track1 = "Single field → Track 1"
    case track2 = "Single field → Track 2"
    case track3 = "Single field → Track 3"
    public var trackOrder: [Int]? {
        switch self {
        case .automatic: return nil
        case .tracks123: return [0, 1, 2]
        case .tracks12: return [0, 1]
        case .tracks23: return [1, 2]
        case .track1: return [0]
        case .track2: return [1]
        case .track3: return [2]
        }
    }
}

public enum KeyboardSwipeParser {
    public static func parse(_ input: String, format: SwipeInputFormat) throws -> CardData {
        // STX/ETX are transport wrappers used by some keyboard-reader configurations.
        var cleaned = input
        if cleaned.last?.isNewline == true { cleaned.removeLast() }
        if cleaned.first == "\u{2}" { cleaned.removeFirst() }
        if cleaned.last == "\u{3}" { cleaned.removeLast() }
        guard let order = format.trackOrder else { return try parse(cleaned) }
        // Character splitting treats CRLF as one newline and preserves empty fields.
        let fields = cleaned.split(omittingEmptySubsequences: false, whereSeparator: { $0.isNewline || $0 == "\t" }).map(String.init)
        guard fields.count == order.count else {
            throw StripeError("This mapping expects \(order.count) field\(order.count == 1 ? "" : "s"), but the reader sent \(fields.count). Choose a matching input format. Fields can be separated by Enter or Tab.")
        }
        var card = CardData()
        for (field, track) in zip(fields, order) { card.tracks[track] = CardData.payload(field, track: track) }
        guard !card.isEmpty else { throw StripeError("The swipe contains no data.") }
        return card
    }
    public static func visibleRaw(_ text: String) -> String {
        text.unicodeScalars.map { value in
            switch value.value {
            case 13: return "\\r"
            case 10: return "\\n\n"
            case 9: return "\\t"
            case 0..<32, 127: return String(format: "\\x%02X", value.value)
            default: return String(value)
            }
        }.joined()
    }
    /// Parses the MSR600 default: %track1?;track2?+track3?<CR>.
    /// A second semicolon is accepted for readers that use ';' for Track 3.
    public static func parse(_ input: String) throws -> CardData {
        let chars = Array(input)
        var index = 0
        var result = CardData()
        var seen = Set<Int>()
        while index < chars.count {
            if chars[index].isNewline || chars[index] == "\t" {
                index += 1; continue
            }
            let track: Int
            switch chars[index] {
            case "%": track = 0
            case "+": track = 2
            case ";": track = seen.contains(1) ? 2 : 1
            default: throw StripeError("Swipe captured without recognizable track markers. Choose an input format below to assign the raw data to tracks.")
            }
            guard !seen.contains(track) else { throw StripeError("Multiple swipes or duplicate tracks received. Clear and swipe one card.") }
            index += 1
            let start = index
            while index < chars.count && chars[index] != "?" { index += 1 }
            guard index < chars.count else { throw StripeError("Incomplete swipe: Track \(track + 1) has no end sentinel (?).") }
            result.tracks[track] = String(chars[start..<index])
            seen.insert(track)
            index += 1
        }
        guard !seen.isEmpty else { throw StripeError("No tracks received.") }
        return result
    }
}
