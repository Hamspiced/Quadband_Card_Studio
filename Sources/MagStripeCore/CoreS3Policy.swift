import Foundation

public enum CoreS3Policy {
    public static func uhf(_ command: UInt8, data: [UInt8]) throws {
        var valid = false
        switch command {
        case 0x03: valid = data == [0]
        case 0x08,0x0B,0x22,0x28,0xB7: valid = data.isEmpty
        case 0x12: valid = data.count == 1 && data[0] <= 2
        case 0xB6: valid = data.count == 2 && (1800...2600).contains(Int(data[0])*256+Int(data[1]))
        case 0x0C:
            if data.count >= 9 {
                let bits = Int(data[5])
                valid = data[0...4] == [1,0,0,0,0x10] && data[6] == 0 && bits >= 32 && bits % 16 == 0 &&
                    data.count == 7 + bits/8 && bits <= 240 && Int(data[7] >> 3) * 2 + 2 == bits/8
            }
        case 0x39,0x49:
            if data.count >= 9 {
                let bank = Int(data[4]), word = Int(data[5])*256+Int(data[6]), count = Int(data[7])*256+Int(data[8])
                valid = bank <= 3 && count > 0 && count <= (command == 0x49 ? 32 : 64) && word+count <= 65536 &&
                    data.count == 9 + (command == 0x49 ? count*2 : 0)
                if command == 0x49 { valid = valid && (bank == 3 || bank == 1 && word >= 1) }
            }
        default: break
        }
        guard valid else { throw StripeError("CoreS3 refused an unsupported or unsafe UHF command.") }
    }
    public static func chameleon(_ command: UInt16, data: [UInt8]) throws {
        try T5577.validateCommand(command,data:data)
        let empty: Set<UInt16> = [1000,1002,1017,1033,1035,2000,3000,3004,3014,3019,3021]
        var valid = empty.contains(command) && data.isEmpty
        if command == 1001 { valid = data.count == 1 && data[0] <= 1 }
        if command == 3002 { valid = data.count == 1 && (data[0] <= 30 || data[0] == 42) }
        if command == 3010 { valid = data == [0] }
        if command == 2008 || command == 2009 {
            valid = data.count == (command == 2008 ? 8 : 24) && [0x60,0x61].contains(data[0])
            if valid && command == 2009 { valid = CardFamily.classic4k.isData(Int(data[1])) }
        }
        if command == 3016 { valid = data.count == 11 }
        guard valid else { throw StripeError("CoreS3 refused an unsupported or unsafe Chameleon command.") }
    }
}
