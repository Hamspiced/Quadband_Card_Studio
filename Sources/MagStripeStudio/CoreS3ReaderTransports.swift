import Foundation
import MagStripeCore

@MainActor final class CoreS3ChameleonTransport: ChameleonTransport {
    private let session: CoreS3Session
    private let generation: UInt32
    private var lease: CoreS3Session.Lease?
    private var closed = false
    var onDisconnect: ((Error) -> Void)?
    init(session: CoreS3Session, generation: UInt32) { self.session = session; self.generation = generation }
    func begin() throws {
        guard !closed, lease == nil,
              session.capabilities?.peripherals.contains(where: { $0.channel == 3 && $0.generation == generation && $0.present }) == true else { throw StripeError("CoreS3 USB Chameleon changed or is busy. Reconnect it explicitly.") }
        lease = try session.acquire(3,capability:"chameleon")
    }
    func end() { if let lease { session.release(lease) }; lease = nil }
    func exchange(_ command: UInt16, data: [UInt8], timeout: TimeInterval) async throws -> ChameleonFrame {
        try CoreS3Policy.chameleon(command,data:data)
        guard !closed else { throw StripeError("CoreS3 Chameleon endpoint is closed.") }
        let temporary = lease == nil
        if temporary { try begin() }; defer { if temporary { end() } }
        guard let lease else { throw StripeError("CoreS3 Chameleon lease unavailable.") }
        let bytes = try ChameleonFrame(command:command,data:data).encoded()
        do {
            let reply = try await session.perform(lease,operation:32,payload:bytes,write:[UInt16(2009),3016].contains(command),timeout:max(timeout,10))
            var decoder = ChameleonDecoder()
            let frames = try decoder.accept(reply)
            guard frames.count == 1, frames[0].command == command, try frames[0].encoded() == reply else { throw StripeError("Invalid bridged Chameleon response.") }
            return frames[0]
        } catch { close(); onDisconnect?(error); throw error }
    }
    func close() { closed = true; end() }
}

@MainActor final class CoreS3M100Transport: SerialUHFTransport {
    private let session: CoreS3Session
    private let generation: UInt32
    private var closed = false
    private var lease: CoreS3Session.Lease?
    var isScoped: Bool { lease != nil }
    var onTag: ((UHFTag) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    init(session: CoreS3Session, generation: UInt32) {
        self.session = session; self.generation = generation
        session.onUHF = { [weak self] bytes, generation in
            guard let self, !self.closed, generation == self.generation else { return }
            do {
                let frame = try self.frame(bytes)
                guard frame.kind == 2, frame.command == 0x22 else { throw StripeError("Unexpected bridge UHF notification.") }
                self.onTag?(try M100Protocol.tag(frame.data))
            } catch { self.close(); self.onDisconnect?(error) }
        }
    }
    func begin() throws {
        guard !closed, lease == nil,
              session.capabilities?.peripherals.contains(where: { $0.channel == 2 && $0.generation == generation && $0.present }) == true else { throw StripeError("CoreS3 UHF module changed or its operation is already scoped.") }
        lease = try session.acquire(2,capability:"m100")
    }
    func end() { if let lease { session.release(lease) }; lease = nil }
    private func frame(_ bytes: [UInt8]) throws -> M100Frame {
        var decoder = M100FrameDecoder()
        let frames = try decoder.accept(bytes)
        guard frames.count == 1, try frames[0].encoded() == bytes else { throw StripeError("Invalid complete bridge M100 frame.") }
        return frames[0]
    }
    func exchange(_ command: UInt8, data: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        try CoreS3Policy.uhf(command,data:data)
        guard !closed,
              session.capabilities?.peripherals.contains(where: { $0.channel == 2 && $0.generation == generation && $0.present }) == true else { throw StripeError("CoreS3 UHF module changed. Reconnect it explicitly.") }
        let temporary = lease == nil
        if temporary { try begin() }; defer { if temporary { end() } }
        guard let lease else { throw StripeError("CoreS3 UHF operation lease is unavailable.") }
        let bytes = try M100Frame(command:command,data:data).encoded()
        do {
            let reply = try frame(await session.perform(lease,operation:32,payload:bytes,write:command == 0x49,timeout:max(timeout,10)))
            if reply.kind == 2 {
                guard command == 0x22, reply.command == 0x22 else { throw StripeError("Unexpected M100 notification in response.") }
                onTag?(try M100Protocol.tag(reply.data)); return []
            }
            if reply.command == 0xFF {
                if command == 0x22 && reply.data == [0x15] { return [] }
                throw M100Protocol.error(reply.data)
            }
            guard reply.command == command else { throw StripeError("M100 bridge response did not match the request.") }
            return reply.data
        } catch { close(); onDisconnect?(error); throw error }
    }
    func close() { closed = true; end(); onTag = nil; session.onUHF = nil }
}
