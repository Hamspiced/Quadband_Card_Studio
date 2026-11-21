import Foundation

public enum CoreS3Protocol {
    public static let service = "b8878a55-59c6-567c-a112-f35a26093466"
    public static let rx = "82a2c3eb-e3c7-50aa-8701-ed1388a3b3a3"
    public static let tx = "37c9a177-5355-5f31-b1e3-808ccf30f92e"
    public static let maxPayload = 2048
    public static func crc(_ bytes: [UInt8]) -> UInt32 {
        var crc = UInt32.max
        for byte in bytes {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = crc & 1 == 0 ? crc >> 1 : (crc >> 1) ^ 0xEDB88320 }
        }
        return crc ^ UInt32.max
    }
}

public struct CoreS3Frame: Equatable {
    public var kind: UInt8
    public var session: UInt64
    public var requestID: UInt32
    public var channel: UInt8
    public var operation: UInt8
    public var status: UInt16
    public var generation: UInt32
    public var payload: [UInt8]
    public init(kind: UInt8 = 0, session: UInt64, requestID: UInt32, channel: UInt8, operation: UInt8,
                status: UInt16 = 0, generation: UInt32 = 0, payload: [UInt8] = []) {
        self.kind = kind; self.session = session; self.requestID = requestID; self.channel = channel
        self.operation = operation; self.status = status; self.generation = generation; self.payload = payload
    }
    public func encoded() throws -> [UInt8] {
        guard kind <= 2, payload.count <= CoreS3Protocol.maxPayload else { throw StripeError("Invalid CoreS3 frame.") }
        var bytes: [UInt8] = [0x51,0x42,1,kind]
        bytes += session.bridgeLE; bytes += requestID.bridgeLE
        bytes += [channel,operation]; bytes += status.bridgeLE; bytes += generation.bridgeLE
        bytes += UInt16(payload.count).bridgeLE; bytes += payload
        bytes += CoreS3Protocol.crc(bytes).bridgeLE
        return bytes
    }
}

public extension FixedWidthInteger {
    var bridgeLE: [UInt8] { (0..<MemoryLayout<Self>.size).map { UInt8(truncatingIfNeeded: self >> ($0 * 8)) } }
}

public struct CoreS3Decoder {
    private var buffer: [UInt8] = []
    private var failed = false
    public init() {}
    public mutating func accept(_ bytes: [UInt8]) throws -> [CoreS3Frame] {
        guard !failed, bytes.count <= 8312 - buffer.count else { failed = true; buffer = []; throw StripeError("CoreS3 receive buffer lost framing. Reconnect.") }
        buffer += bytes
        do {
            var frames: [CoreS3Frame] = []
            while buffer.count >= 4 {
                guard buffer[0] == 0x51, buffer[1] == 0x42, buffer[2] == 1, buffer[3] <= 2 else { throw StripeError("Invalid CoreS3 frame header. Reconnect.") }
                guard buffer.count >= 26 else { break }
                func integer(_ at: Int, _ count: Int) -> UInt64 {
                    (0..<count).reduce(0) { $0 | UInt64(buffer[at + $1]) << ($1 * 8) }
                }
                let length = Int(integer(24,2)), total = 30 + length
                guard length <= CoreS3Protocol.maxPayload else { throw StripeError("Oversized CoreS3 payload.") }
                guard buffer.count >= total else { break }
                guard UInt32(integer(total-4,4)) == CoreS3Protocol.crc(Array(buffer[..<(total-4)])) else { throw StripeError("CoreS3 CRC mismatch. Reconnect.") }
                frames.append(CoreS3Frame(kind:buffer[3],session:integer(4,8),requestID:UInt32(integer(12,4)),
                    channel:buffer[16],operation:buffer[17],status:UInt16(integer(18,2)),generation:UInt32(integer(20,4)),
                    payload:Array(buffer[26..<total-4])))
                buffer.removeFirst(total)
            }
            return frames
        } catch { failed = true; buffer = []; throw error }
    }
}

public struct CoreS3Capabilities: Codable, Equatable {
    public struct Peripheral: Codable, Equatable {
        public let channel: UInt8
        public let generation: UInt32
        public let present: Bool
        public let kind: String
        public let capabilities: [String]
    }
    public let `protocol`: Int
    public let deviceId: String
    public let name: String
    public let firmware: String
    public let clientNonce: String
    public let peripherals: [Peripheral]
    public var mode: String? = nil
    public var owner: String? = nil
    public var modePending: Bool? = nil
    public var remoteAllowed: Bool { (mode ?? "connected") == "connected" && modePending != true }
    public var modeDescription: String {
        if modePending == true { return "Switching CoreS3 modes — readers paused." }
        switch mode {
        case "standalone": return "Standalone mode — readers controlled locally."
        case "home": return "Choose Connected mode on the CoreS3."
        default: return "Connected mode — desktop reader control."
        }
    }
    public func validated() throws -> Self {
        guard self.protocol == 1, !deviceId.isEmpty, peripherals.count == 3,
              Set(peripherals.map(\.channel)) == Set([UInt8(1),2,3]),
              peripherals.allSatisfy({ $0.generation != 0 }),
              clientNonce.count == 16, clientNonce.allSatisfy(\.isHexDigit) else { throw StripeError("Invalid CoreS3 capabilities.") }
        if let mode {
            guard ["home","connected","standalone"].contains(mode),
                  owner == nil || owner == ["home":"none","connected":"desktop","standalone":"local"][mode]
            else { throw StripeError("Invalid CoreS3 mode ownership.") }
        } else if owner != nil || modePending != nil { throw StripeError("CoreS3 mode metadata is incomplete.") }
        return self
    }
}

public struct CoreS3Error: LocalizedError {
    public let status: UInt16
    public let detail: String
    public var errorDescription: String? {
        let names = ["OK","Bad frame","Bad version","Pairing authorization required","Invalid session","Reader unavailable",
                     "Reader changed; refresh and select it again","Bridge busy","Timed out","Cancelled","No card",
                     "Authentication failed","Unsupported operation","Invalid argument","Reader I/O failure",
                     "Write outcome unverified; read the card again","Permission denied","Card data changed"]
        return "CoreS3: " + (Int(status) < names.count ? names[Int(status)] : "Unknown error") + (detail.isEmpty ? "." : ". " + detail)
    }
}
