// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public struct RecordField: Identifiable {
    public let id: String
    public let title: String
    public var placeholder = ""
    public var options: [String] = []
    public var secret = false
    public init(_ id: String, _ title: String, _ placeholder: String = "", options: [String] = [], secret: Bool = false) {
        self.id = id; self.title = title; self.placeholder = placeholder; self.options = options; self.secret = secret
    }
}
public enum RecordKind: String, CaseIterable, Identifiable {
    case text = "Text", url = "URL / URI", customURI = "Custom URL / URI", unitLink = "Unit.Link", search = "Search", payment = "Payment Link", social = "Social Networks", video = "Video Link", file = "File Link", application = "Application", mail = "Mail", contact = "Contact", phone = "Phone", location = "Location", sos = "SOS", bluetooth = "Bluetooth", filament = "3D Printing", wifi = "Wi-Fi", data = "Data"
    public var id: String { rawValue }
    public var fields: [RecordField] {
        switch self {
        case .text: return [.init("text", "Text"), .init("language", "Language", "en")]
        case .url, .customURI, .unitLink, .payment, .social, .video, .file: return [.init("url", self == .customURI ? "URI" : "Link", self == .unitLink ? "https://unit.link/your-name" : self == .customURI ? "myapp://open" : "https://midwestgadgets.org")]
        case .search: return [.init("query", "Search terms"), .init("engine", "Search engine", options: ["Google", "DuckDuckGo", "Bing"])]
        case .application: return [.init("package", "Android package name", "com.example.app")]
        case .mail: return [.init("email", "Email address", "hello@example.com"), .init("subject", "Subject (optional)"), .init("body", "Message (optional)")]
        case .contact: return [.init("name", "Full name"), .init("organization", "Organization (optional)"), .init("phone", "Phone (optional)"), .init("email", "Email (optional)"), .init("url", "Website (optional)")]
        case .phone: return [.init("phone", "Phone number", "+15551234567")]
        case .location: return [.init("latitude", "Latitude", "42.3314"), .init("longitude", "Longitude", "-83.0458")]
        case .sos: return [.init("name", "Name"), .init("contact", "Emergency contact"), .init("phone", "Emergency phone"), .init("medical", "Medical information (optional)"), .init("notes", "Other information (optional)")]
        case .bluetooth: return [.init("address", "Bluetooth address", "00:11:22:33:44:55"), .init("name", "Device name (optional)")]
        case .filament: return [.init("material", "Material", "PLA"), .init("brand", "Brand", "Generic"), .init("color", "Color (hex)", "FFAABB"), .init("minTemp", "Minimum temperature °C", "190"), .init("maxTemp", "Maximum temperature °C", "220")]
        case .wifi: return [.init("ssid", "Network name (SSID)"), .init("security", "Security", options: ["WPA2 Personal", "Open"]), .init("password", "Network password", secret: true)]
        case .data: return [.init("mime", "MIME type", "application/octet-stream"), .init("encoding", "Payload format", options: ["Hexadecimal", "UTF-8 text"]), .init("payload", "Data")]
        }
    }
    public var defaults: [String: String] {
        switch self {
        case .text: return ["language":"en"]
        case .search: return ["engine":"Google"]
        case .wifi: return ["security":"WPA2 Personal"]
        case .data: return ["mime":"application/octet-stream", "encoding":"Hexadecimal"]
        case .filament: return ["material":"PLA", "brand":"Generic", "color":"FFFFFF", "minTemp":"190", "maxTemp":"220"]
        default: return [:]
        }
    }
    public var hint: String {
        switch self {
        case .application: return "Creates an Android Application Record. The receiving device decides how to open the app."
        case .wifi: return "Uses the Wi-Fi WSC format. The password is stored on the tag; automatic connection depends on the phone."
        case .bluetooth: return "Uses Bluetooth Classic OOB format. The device must support pairing; a name and address alone do not bypass pairing."
        case .filament: return "OpenSpool 1.0 JSON record. Your reader/printer must support OpenSpool and the card type."
        case .sos: return "Stores readable emergency information as a Text record."
        case .contact: return "Stores a vCard 3.0 contact."
        case .unitLink: return "Use the URL of an existing Unit.Link profile."
        case .payment: return "Use an existing payment page or payment URI."
        case .data: return "Creates a custom MIME record with exactly the payload you enter."
        case .customURI: return "Use a complete URI, including its scheme. It is stored as data and is not opened by this app."
        default: return "Creates a standard NFC record."
        }
    }
}

public enum RecordBuilder {
    public static func make(_ kind: RecordKind, values: [String: String]) throws -> NDEFRecord {
        func value(_ key: String) -> String { values[key] ?? kind.defaults[key] ?? "" }
        func required(_ key: String, _ label: String) throws -> String {
            let text = value(key).trimmingCharacters(in: .whitespacesAndNewlines)
            guard !text.isEmpty else { throw StripeError("Enter \(label).") }
            return text
        }
        guard values.values.allSatisfy({ $0.utf8.count <= 8192 }) else { throw StripeError("A record field is too long.") }
        switch kind {
        case .text:
            _ = try required("text", "some text")
            return try .text(value("text"), language: required("language", "a language code"))
        case .url, .customURI, .unitLink, .payment, .social, .video, .file:
            let link = try required("url", "a complete link")
            guard let components = URLComponents(string: link), let scheme = components.scheme, !scheme.isEmpty,
                  components.url != nil, !link.contains(where: { $0.isWhitespace }) else { throw StripeError("Enter a complete URI such as https://midwestgadgets.org.") }
            if ![RecordKind.customURI, .payment, .file].contains(kind) {
                guard ["http","https"].contains(scheme.lowercased()), !(components.host ?? "").isEmpty else { throw StripeError("Use a complete http:// or https:// link.") }
            }
            return .uri(link)
        case .search:
            let query = try required("query", "search terms")
            var url = URLComponents(string: value("engine") == "Bing" ? "https://www.bing.com/search" : value("engine") == "DuckDuckGo" ? "https://duckduckgo.com/" : "https://www.google.com/search")!
            url.queryItems = [URLQueryItem(name: "q", value: query)]
            return .uri(url.string!)
        case .application:
            let package = try required("package", "the Android package name")
            guard package.range(of: #"^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$"#, options: .regularExpression) != nil else { throw StripeError("Use an Android package name such as com.example.app.") }
            return NDEFRecord(tnf: 4, type: Array("android.com:pkg".utf8), payload: Array(package.utf8))
        case .mail:
            let email = try required("email", "an email address")
            guard email.contains("@"), !email.contains(where: { $0.isWhitespace || $0 == "?" || $0 == "#" }) else { throw StripeError("Enter a valid email address.") }
            var url = URLComponents(); url.scheme = "mailto"; url.path = email
            let items = [URLQueryItem(name: "subject", value: value("subject")), URLQueryItem(name: "body", value: value("body"))].filter { !($0.value ?? "").isEmpty }
            if !items.isEmpty { url.queryItems = items }
            guard let encoded = url.string else { throw StripeError("The mail record could not be encoded.") }
            return .uri(encoded)
        case .phone:
            let number = try required("phone", "a phone number").filter { !$0.isWhitespace }
            guard number.range(of: #"^\+?[0-9()\-]+$"#, options: .regularExpression) != nil else { throw StripeError("Use a phone number containing digits and an optional country code.") }
            return .uri("tel:" + number)
        case .location:
            guard let lat = Double(value("latitude")), let lon = Double(value("longitude")), lat.isFinite, lon.isFinite, (-90...90).contains(lat), (-180...180).contains(lon) else { throw StripeError("Enter latitude from −90 to 90 and longitude from −180 to 180.") }
            return .uri("geo:\(lat),\(lon)")
        case .contact:
            let name = try required("name", "a contact name")
            func escaped(_ text: String) -> String { text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n").replacingOccurrences(of: "\n", with: "\\n").replacingOccurrences(of: ";", with: "\\;").replacingOccurrences(of: ",", with: "\\,") }
            var lines = ["BEGIN:VCARD", "VERSION:3.0", "N:;\(escaped(name));;;", "FN:\(escaped(name))"]
            for (key, field) in [("organization","ORG"),("phone","TEL"),("email","EMAIL"),("url","URL")] where !value(key).isEmpty { lines.append(field + ":" + escaped(value(key))) }
            lines.append("END:VCARD")
            let text = lines.map(foldVCard).joined(separator: "\r\n") + "\r\n"
            return .mime("text/vcard", Array(text.utf8))
        case .sos:
            let name = try required("name", "a name")
            var text = "EMERGENCY INFORMATION\nName: " + name
            for (key, label) in [("contact","Emergency contact"),("phone","Phone"),("medical","Medical information"),("notes","Notes")] where !value(key).isEmpty { text += "\n\(label): \(value(key))" }
            return try .text(text)
        case .bluetooth:
            let address = try HexBytes.parse(required("address", "the Bluetooth address"), count: 6)
            let name = Array(value("name").utf8)
            guard name.count <= 248 else { throw StripeError("The Bluetooth name is too long.") }
            let extensionData: [UInt8] = name.isEmpty ? [] : [UInt8(name.count + 1),0x09] + name
            let length = 8 + extensionData.count
            return .mime("application/vnd.bluetooth.ep.oob", [UInt8(length & 255),UInt8(length >> 8)] + address.reversed() + extensionData)
        case .wifi:
            let ssid = Array(value("ssid").utf8), password = Array(value("password").utf8), open = value("security") == "Open"
            guard !ssid.isEmpty else { throw StripeError("Enter a network name.") }
            guard ssid.count <= 32 else { throw StripeError("A Wi-Fi network name can contain at most 32 UTF-8 bytes.") }
            guard open || (8...63).contains(password.count) || (password.count == 64 && value("password").allSatisfy(\.isHexDigit)) else { throw StripeError("WPA2 passwords require 8–63 bytes or a 64-digit hexadecimal key.") }
            let credential = WSC.attribute(0x1026,[1]) + WSC.attribute(0x1045,ssid) + WSC.attribute(0x1003,open ? [0,1] : [0,0x20]) + WSC.attribute(0x100F,open ? [0,1] : [0,8]) + WSC.attribute(0x1027,open ? [] : password) + WSC.attribute(0x1020,Array(repeating: 0xFF, count: 6))
            return .mime("application/vnd.wfa.wsc", WSC.attribute(0x104A,[0x10]) + WSC.attribute(0x100E,credential))
        case .filament:
            let material = try required("material", "the filament material"), brand = try required("brand", "the filament brand")
            let color = value("color").trimmingCharacters(in: CharacterSet(charactersIn: "#")).uppercased()
            _ = try HexBytes.parse(color, count: 3)
            guard let low = Int(value("minTemp")), let high = Int(value("maxTemp")), (0...500).contains(low), (low...500).contains(high) else { throw StripeError("Enter valid filament temperature limits from 0 to 500 °C.") }
            let json = ["protocol":"openspool", "version":"1.0", "type":material, "brand":brand, "color_hex":color, "min_temp":String(low), "max_temp":String(high)]
            return .mime("application/json", Array(try JSONSerialization.data(withJSONObject: json, options: [.sortedKeys])))
        case .data:
            let mime = try required("mime", "a MIME type").lowercased()
            guard mime.range(of: #"^[a-z0-9!#$&^_.+-]+/[a-z0-9!#$&^_.+-]+$"#, options: .regularExpression) != nil else { throw StripeError("Use a MIME type such as application/json.") }
            let payload: [UInt8]
            if value("encoding") == "UTF-8 text" { payload = Array(value("payload").utf8) }
            else {
                let compact = value("payload").filter { !$0.isWhitespace && $0 != ":" && $0 != "-" }
                guard compact.count % 2 == 0 else { throw StripeError("Hexadecimal data needs two digits per byte.") }
                payload = try HexBytes.parse(compact, count: compact.count / 2)
            }
            return .mime(mime, payload)
        }
    }
    private static func foldVCard(_ text: String) -> String {
        var result = "", count = 0
        for character in text {
            let size = String(character).utf8.count
            if count + size > 75 { result += "\r\n "; count = 1 }
            result.append(character); count += size
        }
        return result
    }
}
