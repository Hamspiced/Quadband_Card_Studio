// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import MagStripeCore

@MainActor protocol ChameleonByteChannel: AnyObject {
    var onBytes: (([UInt8]) -> Void)? { get set }
    var onDisconnect: ((Error) -> Void)? { get set }
    func send(_ bytes: [UInt8]) throws
    func close()
}
extension SerialChannel: ChameleonByteChannel {}

@MainActor final class ChameleonConnection: ChameleonTransport {
    private let channel: ChameleonByteChannel
    private var decoder = ChameleonDecoder()
    private var pending: CheckedContinuation<ChameleonFrame,Error>?
    private var command: UInt16?
    private var timer: Task<Void,Never>?
    private var closed = false
    var onDisconnect: ((Error) -> Void)?
    init(channel: ChameleonByteChannel) {
        self.channel = channel
        channel.onBytes = { [weak self] in self?.receive($0) }
        channel.onDisconnect = { [weak self] in self?.fail($0) }
    }
    convenience init(path: String) throws { try self.init(channel:SerialChannel(path:path,baud:115200)) }
    func exchange(_ command: UInt16, data: [UInt8], timeout: TimeInterval) async throws -> ChameleonFrame {
        guard !closed else { throw StripeError("Chameleon is disconnected. Reconnect to continue.") }
        guard pending == nil else { throw StripeError("Another Chameleon operation is running.") }
        try Task.checkCancellation()
        let bytes = try ChameleonFrame(command:command,data:data).encoded()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                pending = continuation; self.command = command
                do { try channel.send(bytes) } catch { fail(error); return }
                timer = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds:UInt64(timeout*1e9)) } catch { return }
                    self?.fail(StripeError("Chameleon timed out. Reconnect before trying again."))
                }
            }
        } onCancel: { Task { @MainActor [weak self] in self?.fail(CancellationError()) } }
    }
    private func receive(_ bytes: [UInt8]) {
        guard !closed else { return }
        do {
            for frame in try decoder.accept(bytes) {
                guard frame.command == command, pending != nil else { throw StripeError("Unexpected Chameleon response. Reconnect the reader.") }
                finish(.success(frame))
            }
        } catch { fail(error) }
    }
    private func finish(_ result: Result<ChameleonFrame,Error>) {
        timer?.cancel(); timer = nil; let waiting = pending; pending = nil; command = nil; waiting?.resume(with:result)
    }
    private func fail(_ error: Error) {
        guard !closed else { return }
        closed = true; finish(.failure(error)); channel.onBytes = nil; channel.onDisconnect = nil; channel.close(); onDisconnect?(error)
    }
    func close() {
        guard !closed else { return }
        closed = true; finish(.failure(CancellationError())); channel.onBytes = nil; channel.onDisconnect = nil; channel.close()
    }
}
