// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation

public enum ReaderFamily: String, CaseIterable {
    case acr122 = "ACR122U", acr1552 = "ACR1552U"
    public static func detect(_ name: String) -> Self? {
        let name = name.uppercased()
        guard !name.contains("SAM"), !name.contains("CONTACT READER") else { return nil }
        if name.range(of: #"\bACR1552(U\b|\b)"#, options: .regularExpression) != nil { return .acr1552 }
        if name.range(of: #"\bACR122U?\b"#, options: .regularExpression) != nil { return .acr122 }
        return nil
    }
    public static func isContactless(_ name: String) -> Bool {
        let upper = name.uppercased()
        return !upper.contains("SAM") && (detect(name) != nil || upper.contains("PICC") || upper.contains(" CL "))
    }
}

public enum ReaderRegister: String, CaseIterable {
    case polling, types, pps, ui, hid, characters, keyboard, suspend, host, power, rf, usbSerial, operating, buzzer
    var address: UInt8 {
        switch self {
        case .polling: return 0x23; case .types: return 0x20; case .pps: return 0x24; case .ui: return 0x21
        case .hid: return 0x90; case .characters: return 0x91; case .keyboard: return 0x92; case .suspend: return 0xE5
        case .host: return 0x93; case .power: return 0x50; case .rf: return 0x25; case .usbSerial: return 0xF0
        case .operating: return 0x50; case .buzzer: return 0x52
        }
    }
    public var count: Int { self == .types || self == .hid ? 2 : self == .characters ? 3 : 1 }
    public var readable: Bool { ![.rf,.usbSerial,.buzzer].contains(self) }
    public var replug: Bool { [.polling,.types,.ui,.usbSerial].contains(self) }
    func getCommand(_ family: ReaderFamily) -> [UInt8] {
        family == .acr122 ? [0xFF,0,0x50,0,0] : [0xE0,0,self == .types ? 1 : 0,address,0]
    }
    func setCommand(_ family: ReaderFamily, _ value: [UInt8]) -> [UInt8] {
        if family == .acr122 {
            switch self {
            case .operating: return [0xFF,0,0x51,value[0],0]
            case .buzzer: return [0xFF,0,0x52,value[0],0]
            case .rf: return [0xFF,0,0,0,4,0xD4,0x32,1,value[0]]
            default: return []
            }
        }
        let bytes = self == .usbSerial ? [0] + value : value
        return [0xE0,0,self == .types ? 1 : 0,address,UInt8(bytes.count)] + bytes
    }
}

public struct ReaderChoice: Identifiable {
    public let value: Int
    public let label: String
    public var id: Int { value }
    public init(_ value: Int, _ label: String) { self.value = value; self.label = label }
}

public struct ReaderSetting: Identifiable {
    public let id: String
    public let title: String
    public let group: String
    public let register: ReaderRegister
    public let index: Int
    public let mask: UInt8
    public let choices: [ReaderChoice]
    public let note: String
    public var requirement: String? = nil
    public init(_ id: String, _ title: String, _ group: String, _ register: ReaderRegister, index: Int = 0, mask: UInt8 = 255, choices: [ReaderChoice], note: String = "", requirement: String? = nil) {
        self.id = id; self.title = title; self.group = group; self.register = register; self.index = index; self.mask = mask; self.choices = choices; self.note = note; self.requirement = requirement
    }
    public func value(in snapshot: ReaderSettingsSnapshot) -> Int? {
        guard let bytes = snapshot.values[register], index < bytes.count else { return nil }
        if id == "pulse" { return bytes[0] & 2 == 0 ? 0 : [250,500,1000,2500][Int((bytes[0] >> 4) & 3)] }
        return Int(bytes[index] & mask)
    }
    public func label(_ value: Int?) -> String { choices.first { $0.value == value }?.label ?? value.map { String(format: "Unknown (%02X)", $0) } ?? "Not reported by reader" }
    public func unavailable(in snapshot: ReaderSettingsSnapshot) -> String? {
        if let requirement, !snapshot.supports(requirement) { return "Requires \(requirement == "power" ? "ACR1552U-M firmware 1.03.03+" : requirement == "newUI" ? "firmware 1.06.00+" : "firmware 1.05.00+ (A series: 5.01.00+)")." }
        if register.readable && snapshot.values[register] == nil { return snapshot.errors[register] ?? "This setting could not be read." }
        return nil
    }
}

public struct ReaderSettingsSnapshot {
    public let reader: String
    public let family: ReaderFamily
    public let firmware: String
    public var serial: [UInt8]?
    public var values: [ReaderRegister:[UInt8]] = [:]
    public var errors: [ReaderRegister:String] = [:]
    public init(reader: String, family: ReaderFamily, firmware: String, serial: [UInt8]? = nil) { self.reader = reader; self.family = family; self.firmware = firmware; self.serial = serial }
    public func supports(_ feature: String) -> Bool {
        guard family == .acr1552,
              let range = firmware.range(of: #"\d+\.\d+\.\d+"#, options: .regularExpression) else { return false }
        let parts = firmware[range].split(separator: ".").compactMap { Int($0) }
        guard parts.count == 3 else { return false }
        let value = parts[1] * 100 + parts[2]
        if feature == "power" { return parts[0] == 1 && value >= 303 }
        if feature == "newUI" { return parts[0] == 1 && value >= 600 }
        return (parts[0] == 1 && value >= 500) || (parts[0] == 5 && value >= 100)
    }
}

public enum ReaderSettingsCatalog {
    public static let sourceURL = "https://forum.dangerousthings.com/t/acr1552u-change-rf-power-level-duty-cycle/28545"
    public static func fields(_ family: ReaderFamily) -> [ReaderSetting] {
        func toggle(_ id: String, _ title: String, _ group: String, _ reg: ReaderRegister, _ mask: UInt8, index: Int = 0, requirement: String? = nil) -> ReaderSetting {
            ReaderSetting(id,title,group,reg,index:index,mask:mask,choices:[.init(0,"Off"),.init(Int(mask),"On")],requirement:requirement)
        }
        if family == .acr122 {
            return [
                toggle("buzzer","Card-detection buzzer","Buzzer",.buzzer,255),
                toggle("polling","Automatic card detection","RF & polling",.operating,0x80),
                ReaderSetting("interval","Polling interval","RF & polling",.operating,mask:0x20,choices:[.init(0x20,"250 ms"),.init(0,"500 ms")]),
                ReaderSetting("rf","RF field now","RF & polling",.rf,choices:[.init(0,"Off"),.init(1,"On")],note:"Runtime setting. The polling engine may turn the field on again."),
                toggle("ats","Automatic ATS request","Card detection",.operating,0x40),
                toggle("typeA","ISO 14443 Type A / MIFARE","Card detection",.operating,1),
                toggle("typeB","ISO 14443 Type B","Card detection",.operating,2),
                toggle("topaz","Topaz","Card detection",.operating,4),
                toggle("felica212","FeliCa 212 kbps","Card detection",.operating,8),
                toggle("felica424","FeliCa 424 kbps","Card detection",.operating,16)
            ]
        }
        let delimiters: [ReaderChoice] = [.init(255,"None"),.init(0x28,"Enter"),.init(0x2C,"Space"),.init(0x2D,"Hyphen"),.init(0x33,"Semicolon"),.init(0x36,"Comma"),.init(0x37,"Period")]
        var fields: [ReaderSetting] = [
            toggle("presenceBeep","Card-present buzzer","Buzzer",.ui,8), toggle("removalBeep","Card-removal buzzer","Buzzer",.ui,16),
            toggle("powerBeep","Power-up buzzer","Buzzer",.ui,32,requirement:"newUI"),
            ReaderSetting("power","RF power setting","RF & polling",.power,choices:[.init(0,"Hardware default"),.init(1,"20%"),.init(2,"40%"),.init(3,"60%"),.init(4,"80%"),.init(5,"100%")],note:"The reader restarts after a change. Percentage settings may have no effect on some hardware; this is not a power measurement.",requirement:"power"),
            ReaderSetting("pulse","RF-off interval","RF & polling",.polling,mask:0x32,choices:[.init(0,"Continuous field"),.init(250,"250 ms"),.init(500,"500 ms"),.init(1000,"1,000 ms"),.init(2500,"2,500 ms")],note:"Unplug and reconnect to activate. A continuous field can warm cards left on the reader."),
            toggle("polling","Automatic card detection","RF & polling",.polling,1),
            ReaderSetting("pps","Maximum connection speed","RF & polling",.pps,choices:[.init(0,"106 kbps"),.init(1,"212 kbps"),.init(2,"424 kbps"),.init(3,"848 kbps")]),
            ReaderSetting("rf","RF field now","RF & polling",.rf,choices:[.init(0,"Off"),.init(1,"On, with polling"),.init(2,"On, without polling")],note:"Runtime only; resets when the reader restarts."),
            toggle("mifareATR","MIFARE type identification","Card detection",.polling,8),
            toggle("part4ATR","Part 4 card identification","Card detection",.polling,128),
            toggle("accessLED","Activity LED","Lights",.ui,1),
            toggle("pollingLED","Polling LED","Lights",.ui,2,requirement:"newUI"),
            toggle("activationLED","Card-activation LED","Lights",.ui,4,requirement:"newUI"),
            toggle("suspend","USB selective suspend","USB & keyboard",.suspend,1,requirement:"suspend"),
            ReaderSetting("host","USB mode","USB & keyboard",.host,choices:[.init(1,"Card reader (CCID)"),.init(2,"Card reader + keyboard"),.init(0,"Keyboard only")],note:"Keyboard only removes the PC/SC interface. Returning to card-reader mode requires a separate ACS keyboard configuration utility."),
            toggle("uppercase","Uppercase UID","USB & keyboard",.hid,16),
            ReaderSetting("uidLength","UID lengths","USB & keyboard",.hid,mask:32,choices:[.init(0,"4 bytes only"),.init(32,"4, 7, 8 or 10 bytes")]),
            ReaderSetting("hidMode","UID format","USB & keyboard",.hid,mask:15,choices:["Hexadecimal","Decimal per byte","Decimal","6H-6H","8H-8H","10H-10H","14H-14H","20H-20H","6H-8D","6H-10D","8H-10D","10H-14D","2H4H-8D","14H-17D"].enumerated().map { .init($0.offset,$0.element) }),
            ReaderSetting("hidOrder","UID byte order","USB & keyboard",.hid,index:1,choices:[.init(0,"Normal"),.init(1,"Reversed"),.init(2,"Reverse ISO 14443 / FeliCa"),.init(4,"Reverse ISO 15693")]),
            ReaderSetting("uidStart","Character before UID","USB & keyboard",.characters,index:2,choices:delimiters),
            ReaderSetting("uidBetween","Character between UID bytes","USB & keyboard",.characters,index:0,choices:delimiters),
            ReaderSetting("uidEnd","Character after UID","USB & keyboard",.characters,index:1,choices:delimiters),
            ReaderSetting("keyboard","Keyboard language","USB & keyboard",.keyboard,choices:[.init(0,"English"),.init(1,"French"),.init(3,"Lithuanian")]),
            toggle("usbSerial","Include serial in USB descriptor","USB & keyboard",.usbSerial,1)
        ]
        for (id,title,index,bit) in [("typeA","ISO 14443 Type A",0,0),("typeB","ISO 14443 Type B",0,1),("felica","FeliCa",0,2),("topaz","Topaz",0,4),("innovatron","Innovatron",0,5),("sri","SRI / SRIX",0,6),("picopassB","PicoPass (ISO 14443B)",1,0),("picopass15693","PicoPass (ISO 15693)",1,1),("iso15693","ISO 15693",1,2),("cts","CTS",1,3)] {
            fields.append(toggle(id,title,"Card detection",.types,UInt8(1 << bit),index:index))
        }
        return fields
    }
    public static func preset(_ name: String, family: ReaderFamily, snapshot: ReaderSettingsSnapshot) -> [String:Int] {
        if name == "mute" || name == "unmute" {
            if family == .acr122 { return ["buzzer":name == "mute" ? 0 : 255] }
            var result = ["presenceBeep":name == "mute" ? 0 : 8, "removalBeep":name == "mute" ? 0 : 16]
            if snapshot.supports("newUI") { result["powerBeep"] = name == "mute" ? 0 : 32 }
            return result
        }
        guard family == .acr1552 else { return [:] }
        switch name {
        case "ashsh": return ["power":5,"pulse":0]
        case "tototo31": return ["power":4,"pulse":250]
        case "rfDefaults": return ["power":0,"pulse":250]
        default: return [:]
        }
    }
}

public struct ReaderConfigChange: Identifiable {
    public var id: String { register.rawValue }
    public let register: ReaderRegister
    public let before: [UInt8]?
    public let after: [UInt8]
    public let descriptions: [String]
    public var detail: String {
        if register.replug { return "Saved in reader; unplug and reconnect to activate." }
        if register == .power { return "Reader restarts; the app will try to reconnect and verify." }
        if register == .host && after == [0] { return "Disconnects PC/SC. Requires an external ACS utility to restore card-reader mode." }
        if !register.readable { return "Reader acknowledgement only; this setting has no read-back command." }
        return "Apply and read back."
    }
}

public enum ReaderConfigPlan {
    public static func make(snapshot: ReaderSettingsSnapshot, edits: [String:Int]) throws -> [ReaderConfigChange] {
        let fields = ReaderSettingsCatalog.fields(snapshot.family)
        var desired: [ReaderRegister:[UInt8]] = [:], descriptions: [ReaderRegister:[String]] = [:]
        for (id,value) in edits.sorted(by: { $0.key < $1.key }) {
            guard let field = fields.first(where: { $0.id == id }), field.choices.contains(where: { $0.value == value }) else { throw StripeError("Unknown or invalid reader setting: \(id).") }
            if let reason = field.unavailable(in: snapshot) { throw StripeError("\(field.title): \(reason)") }
            if id == "hidOrder", [2,4].contains(value), !snapshot.supports("suspend") { throw StripeError("Selective UID reversal requires newer firmware.") }
            if field.value(in: snapshot) == value { continue }
            var bytes = desired[field.register] ?? snapshot.values[field.register].map { Array($0.prefix(field.register.count)) } ?? Array(repeating: 0, count: field.register.count)
            guard field.index < bytes.count else { throw StripeError("Incomplete reader setting: \(field.title).") }
            if id == "pulse" {
                if value == 0 { bytes[0] &= ~2 }
                else { bytes[0] = (bytes[0] & ~0x32) | 2 | UInt8([250,500,1000,2500].firstIndex(of: value)! << 4) }
            } else { bytes[field.index] = (bytes[field.index] & ~field.mask) | UInt8(value) }
            desired[field.register] = bytes
            descriptions[field.register,default:[]].append("\(field.title): \(field.label(field.value(in:snapshot))) → \(field.label(value))")
        }
        if snapshot.family == .acr1552 {
            if let types = desired[.types], types[0] & 0x77 == 0 && types[1] & 15 == 0 { throw StripeError("Keep at least one card technology enabled.") }
            if let types = desired[.types] { desired[.types] = [types[0] & 0x77,types[1] & 15] }
            let host = desired[.host]?.first ?? snapshot.values[.host]?.first
            let suspend = desired[.suspend]?.first ?? snapshot.values[.suspend]?.first
            if (desired[.host] != nil || desired[.suspend] != nil), host != 1, suspend == 1 { throw StripeError("Turn off USB selective suspend before enabling keyboard mode.") }
            if desired[.suspend] == [1], host == nil { throw StripeError("Read the USB mode before enabling selective suspend.") }
            if let host = desired[.host]?.first, host != 1, snapshot.supports("suspend"), suspend == nil { throw StripeError("Read the suspend setting before enabling keyboard mode.") }
        } else if let value = desired[.operating]?.first, value & 0x80 != 0, value & 0x1F == 0 { throw StripeError("Keep at least one card technology enabled while polling.") }
        var order: [ReaderRegister] = [.polling,.types,.pps,.ui,.hid,.characters,.keyboard,.operating,.buzzer,.usbSerial]
        // Disabling suspend must precede enabling HID; disabling HID precedes enabling suspend.
        if desired[.suspend] == [0] { order += [.suspend,.host] } else { order += [.host,.suspend] }
        order += [.power,.rf]
        if desired[.host] == [0] { order.removeAll { $0 == .host }; order.append(.host) }
        return order.compactMap { register in
            guard let bytes = desired[register] else { return nil }
            return ReaderConfigChange(register:register,before:snapshot.values[register].map { Array($0.prefix(register.count)) },after:bytes,descriptions:descriptions[register] ?? [])
        }
    }
}

public protocol ReaderControlTransport: Sendable { func control(_ command: [UInt8]) async throws -> [UInt8] }
public struct ReaderApplyResult {
    public let title: String
    public let success: Bool
    public let verified: Bool
    public let detail: String
}

public struct ReaderConfigurationClient {
    let transport: any ReaderControlTransport
    public init(transport: any ReaderControlTransport) { self.transport = transport }
    public static func escapePayload(_ reply: [UInt8]) throws -> [UInt8] {
        guard reply.count >= 5, Array(reply.prefix(4)) == [0xE1,0,0,0], reply.count == 5 + Int(reply[4]) else { throw StripeError("Reader returned an unsupported or incomplete configuration response.") }
        return Array(reply.dropFirst(5))
    }
    public static func operatingByte(_ reply: [UInt8]) throws -> [UInt8] {
        if reply.count == 1 { return reply }
        if reply.count == 2 && reply[0] == 0x90 { return [reply[1]] }
        throw StripeError("The ACR122U did not return a valid polling parameter.")
    }
    private func firmware(_ family: ReaderFamily) async throws -> String {
        let raw = try await transport.control(family == .acr122 ? [0xFF,0,0x48,0,0] : [0xE0,0,0,0x18,0])
        let bytes = try family == .acr1552 ? Self.escapePayload(raw) : raw
        guard bytes.count <= 80, let string = String(bytes:bytes,encoding:.ascii), string.utf8.allSatisfy({ (32..<127).contains($0) }),
              string.hasPrefix(family == .acr122 ? "ACR122U" : "ACR1552") else { throw StripeError("The selected reader did not identify as \(family.rawValue). No configuration was changed.") }
        return string
    }
    private func readRegister(_ reg: ReaderRegister, family: ReaderFamily) async throws -> [UInt8] {
        let raw = try await transport.control(reg.getCommand(family))
        let data = try family == .acr122 ? Self.operatingByte(raw) : Self.escapePayload(raw)
        guard data.count == reg.count || (reg == .pps && data.count == 2) else { throw StripeError("The reader returned an unexpected setting length.") }
        return data
    }
    public func read(reader: String) async throws -> ReaderSettingsSnapshot {
        guard let family = ReaderFamily.detect(reader) else { throw StripeError("Choose an ACR122U or ACR1552U contactless interface, not a SAM slot.") }
        var snapshot = ReaderSettingsSnapshot(reader:reader,family:family,firmware:try await firmware(family))
        if family == .acr1552 { snapshot.serial = try? await Self.escapePayload(transport.control([0xE0,0,0,0x33,0])) }
        let registers: [ReaderRegister] = family == .acr122 ? [.operating] : [.polling,.types,.pps,.ui,.hid,.characters,.keyboard,.host,.power,.suspend]
        for register in registers {
            try Task.checkCancellation()
            if register == .power && !snapshot.supports("power") { continue }
            if register == .suspend && !snapshot.supports("suspend") { continue }
            do { snapshot.values[register] = try await readRegister(register,family:family) }
            catch { snapshot.errors[register] = error.localizedDescription }
        }
        return snapshot
    }
    public func apply(snapshot: ReaderSettingsSnapshot, edits: [String:Int]) async throws -> [ReaderApplyResult] {
        let plan = try ReaderConfigPlan.make(snapshot:snapshot,edits:edits)
        guard !plan.isEmpty else { return [] }
        guard try await firmware(snapshot.family) == snapshot.firmware else { throw StripeError("The reader firmware changed. Read its settings again.") }
        if let serial = snapshot.serial {
            let actual = try await Self.escapePayload(transport.control([0xE0,0,0,0x33,0]))
            guard actual == serial else { throw StripeError("The reader changed. Read its settings again.") }
        }
        // Check all touched registers before the first mutation, including shared bitfields.
        for change in plan where change.register.readable {
            let current = try await readRegister(change.register,family:snapshot.family)
            guard Array(current.prefix(change.register.count)) == change.before else { throw StripeError("Reader settings changed since they were loaded. Read settings again before applying.") }
        }
        // Host/suspend constraints also depend on an unchanged counterpart.
        if snapshot.family == .acr1552 && plan.contains(where: { [.host,.suspend].contains($0.register) }) {
            for register in [ReaderRegister.host,.suspend] {
                if let before = snapshot.values[register] {
                    guard try await readRegister(register,family:snapshot.family) == before else { throw StripeError("USB mode or suspend changed. Read settings again.") }
                }
            }
        }
        var results: [ReaderApplyResult] = []
        for change in plan {
            try Task.checkCancellation()
            let title = change.descriptions.joined(separator:"; ")
            do {
                var writeError: Error?
                do {
                    let reply = try await transport.control(change.register.setCommand(snapshot.family,change.after))
                    if snapshot.family == .acr1552 {
                        let payload = try Self.escapePayload(reply)
                        let expected = change.register == .usbSerial ? change.after + [0x90,0] : change.after
                        guard payload == expected else { throw StripeError("Reader did not acknowledge the requested value.") }
                    } else if change.register == .operating {
                        guard try Self.operatingByte(reply) == change.after else { throw StripeError("Reader did not confirm the polling value.") }
                    } else {
                        let expected: [UInt8] = change.register == .rf ? [0xD5,0x33,0x90,0] : [0x90,0]
                        guard reply == expected else { throw StripeError("Reader did not acknowledge the command.") }
                    }
                } catch { writeError = error }
                if change.register == .power {
                    // This command can disconnect before its acknowledgement arrives. Never resend it.
                    var matched = false
                    for _ in 0..<20 {
                        try await Task.sleep(nanoseconds:250_000_000)
                        if let value = try? await readRegister(.power,family:snapshot.family), value == change.after {
                            guard try await firmware(snapshot.family) == snapshot.firmware else { throw StripeError("Reader identity changed after restart.") }
                            if let serial = snapshot.serial {
                                let actual = try await Self.escapePayload(transport.control([0xE0,0,0,0x33,0]))
                                guard actual == serial else { throw StripeError("A different reader appeared after restart.") }
                            }
                            matched = true; break
                        }
                    }
                    guard matched else { throw StripeError("RF power was sent, but could not be verified after the reader restarted. Reconnect and read settings; the command was not retried.") }
                } else {
                    if let writeError { throw writeError }
                    if change.register.readable {
                        let actual = try await readRegister(change.register,family:snapshot.family)
                        guard Array(actual.prefix(change.register.count)) == change.after else { throw StripeError("The setting did not match on read-back.") }
                    }
                }
                results.append(ReaderApplyResult(title:title,success:true,verified:change.register.readable,detail:change.detail))
            } catch {
                results.append(ReaderApplyResult(title:title,success:false,verified:false,detail:"Not verified: \(error.localizedDescription) Earlier changes may already be applied. Remaining changes were not sent."))
                break
            }
        }
        return results
    }
}
