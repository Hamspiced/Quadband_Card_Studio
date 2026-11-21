import Foundation

@MainActor public protocol CoreS3ByteChannel: AnyObject {
    var onBytes: (([UInt8]) -> Void)? { get set }
    var onDisconnect: ((Error) -> Void)? { get set }
    func send(_ bytes: [UInt8]) throws
    func close()
}

@MainActor public final class CoreS3Session {
    public struct Lease: Equatable {
        public let channel: UInt8
        public let generation: UInt32
        fileprivate let identity: UUID
    }
    public func refreshCapabilities() async throws -> CoreS3Capabilities {
        guard lease == nil else { throw StripeError("Finish the active reader operation before refreshing CoreS3 modules.") }
        _ = try await request(channel:0,operation:2,payload:[])
        guard let capabilities else { throw StripeError("CoreS3 is disconnected.") }; return capabilities
    }
    public func ping(_ payload: [UInt8] = []) async throws -> [UInt8] {
        guard lease == nil, payload.count <= 64 else { throw StripeError("CoreS3 ping requires an idle session and at most 64 bytes.") }
        let result = try await request(channel:0,operation:3,payload:payload).payload
        guard result == payload else { let error = StripeError("CoreS3 ping response mismatch."); fail(error); throw error }
        return result
    }
    private let channel: any CoreS3ByteChannel
    private var decoder = CoreS3Decoder()
    private var nonce: UInt64 = 0
    private var sequence: UInt32 = 0
    private var closed = false
    private var lease: Lease?
    private var pending: (CoreS3Frame, Bool, CheckedContinuation<CoreS3Frame,Error>)?
    private var timer: Task<Void,Never>?
    public private(set) var capabilities: CoreS3Capabilities?
    public var onCapabilities: ((CoreS3Capabilities) -> Void)?
    public var onDisconnect: ((Error) -> Void)?
    public var onUHF: (([UInt8], UInt32) -> Void)?
    public init(channel: any CoreS3ByteChannel) {
        self.channel = channel
        channel.onBytes = { [weak self] in self?.receive($0) }
        channel.onDisconnect = { [weak self] in self?.fail($0) }
    }
    public func hello(timeout: TimeInterval = 90) async throws -> CoreS3Capabilities {
        guard nonce == 0 else { throw StripeError("CoreS3 session is already established.") }
        let clientNonce = (0..<8).map { _ in UInt8.random(in:0...255) }
        _ = try await request(channel:0,operation:1,payload:clientNonce,timeout:timeout)
        guard let capabilities else { throw StripeError("CoreS3 HELLO did not establish capabilities.") }
        return capabilities
    }
    public func acquire(_ channel: UInt8, capability: String? = nil) throws -> Lease {
        guard capabilities?.remoteAllowed == true else { throw StripeError("Select Connected mode on the CoreS3 before using its readers.") }
        guard !closed, nonce != 0,
              let slot = capabilities?.peripherals.first(where: { $0.channel == channel && $0.present }),
              capability == nil || slot.capabilities.contains(capability!) else { throw StripeError("CoreS3 endpoint is busy, unavailable or unsupported.") }
        guard lease == nil else { throw StripeError("CoreS3 is in use. Stop scanning or finish the current reader operation first.") }
        let value = Lease(channel:channel,generation:slot.generation,identity:UUID()); lease = value; return value
    }
    public func release(_ value: Lease) { if lease == value { lease = nil } }
    public func perform(_ value: Lease, operation: UInt8, payload: [UInt8] = [], write: Bool = false, timeout: TimeInterval = 15) async throws -> [UInt8] {
        guard capabilities?.remoteAllowed == true, lease == value,
              capabilities?.peripherals.contains(where: { $0.channel == value.channel && $0.generation == value.generation && $0.present }) == true else { throw StripeError("CoreS3 reader changed. Refresh and explicitly select it again.") }
        return try await request(channel:value.channel,operation:operation,generation:value.generation,payload:payload,write:write,timeout:timeout).payload
    }
    private func request(channel: UInt8, operation: UInt8, generation: UInt32 = 0, payload: [UInt8], write: Bool = false, timeout: TimeInterval = 15) async throws -> CoreS3Frame {
        guard !closed, pending == nil, sequence < UInt32.max,
              timeout.isFinite, timeout > 0, timeout <= ((channel == 1 && [UInt8(22),23].contains(operation)) ? 120 : 90),
              operation == 1 || nonce != 0 else { throw StripeError("CoreS3 session is busy, disconnected or has an invalid timeout. Reconnect.") }
        try Task.checkCancellation()
        sequence += 1
        let frame = CoreS3Frame(session:nonce,requestID:sequence,channel:channel,operation:operation,generation:generation,payload:payload)
        let bytes = try frame.encoded()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                pending = (frame,write,continuation)
                timer = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds:UInt64(timeout * 1e9)) } catch { return }
                    self?.fail(StripeError("CoreS3 timed out. Reconnect before retrying."))
                }
                do { try self.channel.send(bytes) } catch { fail(error) }
            }
        } onCancel: { Task { @MainActor [weak self] in self?.fail(CancellationError()) } }
    }
    private func update(_ value: CoreS3Capabilities) {
        if let lease, !value.peripherals.contains(where: { $0.channel == lease.channel && $0.generation == lease.generation && $0.present }) {
            self.lease = nil
            if pending?.0.channel == lease.channel { finish(.failure(StripeError("CoreS3 peripheral changed during operation. Any transmitted write is unverified."))) }
        }
        if !value.remoteAllowed && (pending == nil || pending?.0.channel == 0) { lease = nil }
        capabilities = value; onCapabilities?(value)
    }
    private func receive(_ bytes: [UInt8]) {
        guard !closed else { return }
        do {
            for frame in try decoder.accept(bytes) {
                if frame.kind == 2 {
                    guard frame.requestID == 0, nonce != 0, frame.session == nonce, frame.status == 0 else { throw StripeError("Invalid CoreS3 event.") }
                    if frame.channel == 0, frame.operation == 128, frame.generation == 0 {
                        let value = try JSONDecoder().decode(CoreS3Capabilities.self,from:Data(frame.payload)).validated()
                        guard value.clientNonce == capabilities?.clientNonce, value.deviceId == capabilities?.deviceId else { throw StripeError("CoreS3 event identity mismatch.") }
                        update(value)
                    } else if frame.channel == 2, frame.operation == 32,
                              capabilities?.peripherals.contains(where: { $0.channel == 2 && $0.present && $0.generation == frame.generation }) == true {
                        onUHF?(frame.payload,frame.generation)
                    } else { throw StripeError("Unexpected CoreS3 event.") }
                    continue
                }
                guard let (request,_,_) = pending, frame.kind == 1, frame.requestID == request.requestID,
                      frame.channel == request.channel, frame.operation == request.operation,
                      frame.generation == request.generation,
                      (request.operation == 1 ? (frame.status != 0 || frame.session != 0) : frame.session == nonce) else { throw StripeError("Uncorrelated CoreS3 reply. Reconnect.") }
                if frame.status == 0 {
                    if request.channel == 0 && [UInt8(1),2].contains(request.operation) {
                        let value = try JSONDecoder().decode(CoreS3Capabilities.self,from:Data(frame.payload)).validated()
                        if request.operation == 1 {
                            guard value.clientNonce == request.payload.map({ String(format:"%02x",$0) }).joined() else { throw StripeError("CoreS3 HELLO nonce mismatch.") }
                            nonce = frame.session
                        } else {
                            guard value.clientNonce == capabilities?.clientNonce, value.deviceId == capabilities?.deviceId else { throw StripeError("CoreS3 capability identity mismatch.") }
                        }
                        update(value)
                    }
                    finish(.success(frame))
                }
                else {
                    let error = CoreS3Error(status:frame.status,detail:String(bytes:frame.payload,encoding:.utf8) ?? "")
                    if (1...4).contains(frame.status) { fail(error); return }
                    finish(.failure(error))
                }
            }
        } catch { fail(error) }
    }
    private func finish(_ result: Result<CoreS3Frame,Error>) {
        timer?.cancel(); timer = nil; let waiting = pending; pending = nil; waiting?.2.resume(with:result)
        if capabilities?.remoteAllowed == false { lease = nil }
    }
    private func fail(_ error: Error) {
        guard !closed else { return }
        let outcome: Error = pending?.1 == true ? StripeError("Write outcome unverified. Read the target before retrying. \(error.localizedDescription)") : error
        closed = true; finish(.failure(outcome)); nonce = 0; lease = nil; capabilities = nil; decoder = CoreS3Decoder()
        channel.onBytes = nil; channel.onDisconnect = nil; channel.close(); onDisconnect?(outcome)
    }
    public func close() { fail(StripeError("CoreS3 disconnected.")) }
}
