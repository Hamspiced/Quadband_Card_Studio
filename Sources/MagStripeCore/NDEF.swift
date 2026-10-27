// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct NDEFRecord: Equatable, Identifiable {
    public let id = UUID()
    public var tnf: UInt8
    public var type: [UInt8]
    public var identifier: [UInt8]
    public var payload: [UInt8]
    public init(tnf: UInt8, type: [UInt8], identifier: [UInt8] = [], payload: [UInt8]) {
        self.tnf = tnf; self.type = type; self.identifier = identifier; self.payload = payload
    }
    public static func == (lhs: Self, rhs: Self) -> Bool { lhs.tnf == rhs.tnf && lhs.type == rhs.type && lhs.identifier == rhs.identifier && lhs.payload == rhs.payload }
    public static let uriPrefixes = ["", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://", "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:"]
    public static func uri(_ value: String) -> Self {
        let match = uriPrefixes.enumerated().filter { !$0.element.isEmpty && value.hasPrefix($0.element) }.max { $0.element.count < $1.element.count }
        return Self(tnf: 1, type: [0x55], payload: [UInt8(match?.offset ?? 0)] + Array(value.dropFirst(match?.element.count ?? 0).utf8))
    }
    public static func text(_ value: String, language: String = "en") throws -> Self {
        guard !language.isEmpty, language.utf8.count <= 63, language.utf8.allSatisfy({ (65...90).contains($0) || (97...122).contains($0) || $0 == 45 }) else { throw StripeError("Use a language code such as en or en-US.") }
        return Self(tnf: 1, type: [0x54], payload: [UInt8(language.utf8.count)] + Array(language.utf8) + Array(value.utf8))
    }
    public static func mime(_ type: String, _ payload: [UInt8]) -> Self { Self(tnf: 2, type: Array(type.utf8), payload: payload) }
    public var typeName: String { String(decoding: type, as: UTF8.self) }
    public var uriValue: String? {
        guard tnf == 1, type == [0x55], let prefix = payload.first, Int(prefix) < Self.uriPrefixes.count,
              let rest = String(bytes: payload.dropFirst(), encoding: .utf8) else { return nil }
        return Self.uriPrefixes[Int(prefix)] + rest
    }
    public var textValue: String? {
        guard tnf == 1, type == [0x54], let status = payload.first, status & 0x40 == 0 else { return nil }
        let start = 1 + Int(status & 0x3F)
        guard start <= payload.count else { return nil }
        return String(data: Data(payload.dropFirst(start)), encoding: status & 0x80 == 0 ? .utf8 : .utf16)
    }
    public var title: String {
        if uriValue != nil { return "URL / URI" }
        if textValue != nil { return "Text" }
        switch typeName {
        case "text/vcard", "text/x-vcard": return "Contact"
        case "android.com:pkg": return "Application"
        case "application/vnd.wfa.wsc": return "Wi-Fi network"
        case "application/vnd.bluetooth.ep.oob": return "Bluetooth connection"
        case "application/json": return "JSON data / filament"
        default: return typeName.isEmpty ? "Data" : typeName
        }
    }
    public var displayValue: String {
        if let uriValue { return uriValue }
        if let textValue { return textValue }
        if typeName == "application/vnd.bluetooth.ep.oob", payload.count >= 8 {
            let address = payload[2..<8].reversed().map { String(format: "%02X", $0) }.joined(separator: ":")
            return "Bluetooth address: " + address
        }
        if typeName == "application/vnd.wfa.wsc", let ssid = WSC.ssid(payload) { return "Network: " + ssid + "\nConnection settings stored on the tag." }
        if let value = String(bytes: payload, encoding: .utf8), !value.unicodeScalars.contains(where: { $0.value < 32 && ![9,10,13].contains($0.value) }) { return value }
        return HexBytes.string(payload)
    }
}

public enum NDEFCodec {
    public static func encode(_ records: [NDEFRecord]) throws -> [UInt8] {
        guard !records.isEmpty, records.count <= 64 else { throw StripeError("Add between 1 and 64 NFC records.") }
        var out: [UInt8] = []
        for (index, record) in records.enumerated() {
            guard record.type.count <= 255, record.identifier.count <= 255, record.payload.count <= 1_048_576,
                  record.tnf <= 5, !(record.tnf == 0 && (!record.type.isEmpty || !record.payload.isEmpty || !record.identifier.isEmpty)),
                  !(record.tnf == 5 && !record.type.isEmpty), !(record.tnf > 0 && record.tnf < 5 && record.type.isEmpty) else { throw StripeError("An NFC record has an invalid type, identifier or size.") }
            let short = record.payload.count < 256
            out.append(record.tnf | (index == 0 ? 0x80 : 0) | (index == records.count - 1 ? 0x40 : 0) | (short ? 0x10 : 0) | (record.identifier.isEmpty ? 0 : 8))
            out.append(UInt8(record.type.count))
            if short { out.append(UInt8(record.payload.count)) }
            else { let size = UInt32(record.payload.count); out += [UInt8(size >> 24), UInt8((size >> 16) & 255), UInt8((size >> 8) & 255), UInt8(size & 255)] }
            if !record.identifier.isEmpty { out.append(UInt8(record.identifier.count)) }
            out += record.type + record.identifier + record.payload
        }
        guard out.count <= 1_048_576 else { throw StripeError("NDEF message is too large.") }
        return out
    }
    public static func decode(_ bytes: [UInt8]) throws -> [NDEFRecord] {
        if bytes.isEmpty { return [] }
        guard bytes.count <= 1_048_576 else { throw StripeError("NDEF message is too large.") }
        var cursor = 0, records: [NDEFRecord] = [], ended = false
        func take(_ n: Int) throws -> [UInt8] {
            guard n >= 0, n <= bytes.count - cursor else { throw StripeError("The NFC record is truncated.") }
            defer { cursor += n }; return Array(bytes[cursor..<cursor + n])
        }
        while cursor < bytes.count {
            let header = try take(1)[0], typeLength = Int(try take(1)[0])
            guard (header & 0x80 != 0) == records.isEmpty, header & 0x20 == 0, !ended else { throw StripeError("Unsupported chunked or malformed NDEF message.") }
            let lengthBytes = try take(header & 0x10 != 0 ? 1 : 4)
            let length = lengthBytes.reduce(0) { $0 * 256 + Int($1) }
            let idLength = header & 8 != 0 ? Int(try take(1)[0]) : 0
            let type = try take(typeLength), id = try take(idLength), payload = try take(length)
            let record = NDEFRecord(tnf: header & 7, type: type, identifier: id, payload: payload)
            _ = try encode([record])
            records.append(record); ended = header & 0x40 != 0
            guard records.count <= 64 else { throw StripeError("This tag has too many records.") }
        }
        guard ended else { throw StripeError("The NFC message is missing its final record marker.") }
        return records
    }
}

public enum WSC {
    public static func attribute(_ type: UInt16, _ value: [UInt8]) -> [UInt8] {
        [UInt8(type >> 8), UInt8(type & 255), UInt8(value.count >> 8), UInt8(value.count & 255)] + value
    }
    public static func fields(_ bytes: [UInt8]) -> [(UInt16, [UInt8])]? {
        var cursor = 0, values: [(UInt16, [UInt8])] = []
        while cursor < bytes.count {
            guard cursor + 4 <= bytes.count else { return nil }
            let type = UInt16(bytes[cursor]) << 8 | UInt16(bytes[cursor + 1]), size = Int(bytes[cursor + 2]) * 256 + Int(bytes[cursor + 3])
            cursor += 4; guard size <= bytes.count - cursor else { return nil }
            values.append((type, Array(bytes[cursor..<cursor + size]))); cursor += size
        }
        return values
    }
    public static func ssid(_ bytes: [UInt8]) -> String? {
        guard let credential = fields(bytes)?.first(where: { $0.0 == 0x100E })?.1,
              let name = fields(credential)?.first(where: { $0.0 == 0x1045 })?.1 else { return nil }
        return String(bytes: name, encoding: .utf8)
    }
}
