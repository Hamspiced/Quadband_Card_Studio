// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import MagStripeCore

@MainActor final class PiswordsTransport: UHFTransport {
    private let channel: SerialChannel
    let address: UInt8
    private var decoder = UHFFrameDecoder()
    private var pending: CheckedContinuation<[UInt8],Error>?
    private var pendingCommand: UInt8?
    private var timer: Task<Void,Never>?
    var onTag: ((UHFTag) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    var scanning = false
    init(path: String, baud: Int, address: UInt8) throws {
        self.address = address
        channel = try SerialChannel(path:path,baud:baud)
        channel.onBytes = { [weak self] in self?.received($0) }
        channel.onDisconnect = { [weak self] error in self?.finish(.failure(error)); self?.onDisconnect?(error) }
    }
    private func received(_ bytes: [UInt8]) {
        do {
            for frame in try decoder.accept(bytes) {
                guard frame.address == address else { continue }
                if frame.command == 0x89, frame.data.count > 2 { onTag?(try UHFProtocol.tag(frame.data)) }
                if frame.command == pendingCommand {
                    // Inventory notifications can precede the command acknowledgement.
                    if frame.command == 0x89 && frame.data.count > 2 { continue }
                    finish(.success(frame.data))
                } else if frame.command == 0x89, frame.data.count == 1, ![0x10,0x36].contains(frame.data[0]) {
                    throw UHFProtocol.error(frame.data[0])
                }
            }
        } catch { finish(.failure(error)); close(); onDisconnect?(error) }
    }
    func exchange(_ command: UInt8, data: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        guard pending == nil else { throw StripeError("Another UHF command is running.") }
        try Task.checkCancellation()
        let bytes = try UHFFrame(address:address,command:command,data:data).encoded()
        if command == 0x89 { scanning = true }
        let reply = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<[UInt8],Error>) in
                pending = continuation; pendingCommand = command
                do { try channel.send(bytes) } catch { finish(.failure(error)); return }
                timer = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds:UInt64(timeout*1e9)) } catch { return }
                    let error = StripeError("UHF reader timed out. Reconnect and check the port, baud rate and address.")
                    // Closing prevents a delayed acknowledgement from satisfying a later command.
                    self?.finish(.failure(error)); self?.close(); self?.onDisconnect?(error)
                }
            }
        } onCancel: { Task { @MainActor [weak self] in self?.close() } }
        if command == 0x8C { scanning = false }
        return reply
    }
    private func finish(_ result: Result<[UInt8],Error>) { timer?.cancel(); timer = nil; let c = pending; pending = nil; pendingCommand = nil; c?.resume(with:result) }
    func close() {
        if scanning, let bytes = try? UHFFrame(address:address,command:0x8C).encoded() { try? channel.send(bytes) }
        scanning = false; onTag = nil
        finish(.failure(CancellationError())); channel.close()
    }
}
