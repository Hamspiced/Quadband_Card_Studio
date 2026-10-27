// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import Darwin
import IOKit
import MagStripeCore

struct SerialDevice: Identifiable, Equatable {
    var id: String { path }
    let path: String
    let name: String
    let vendor: Int?
    let product: Int?
    var label: String { "\(name) · \(URL(fileURLWithPath:path).lastPathComponent)" }
    static func scan() -> [SerialDevice] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOSerialBSDClient"), &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var result: [SerialDevice] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            func property(_ entry: io_registry_entry_t, _ key: String) -> Any? { IORegistryEntryCreateCFProperty(entry,key as CFString,kCFAllocatorDefault,0)?.takeRetainedValue() }
            guard let path = property(service,"IOCalloutDevice") as? String, path.hasPrefix("/dev/cu."), !path.contains("Bluetooth"), !path.contains("debug-console") else { continue }
            var entry = service, owned = false, name: String?, vendor: Int?, product: Int?
            for _ in 0..<12 {
                name = name ?? property(entry,"USB Product Name") as? String
                vendor = vendor ?? (property(entry,"idVendor") as? NSNumber)?.intValue
                product = product ?? (property(entry,"idProduct") as? NSNumber)?.intValue
                if vendor != nil { break }
                var parent: io_registry_entry_t = 0
                guard IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent) == KERN_SUCCESS else { break }
                if owned { IOObjectRelease(entry) }
                entry = parent; owned = true
            }
            if owned { IOObjectRelease(entry) }
            result.append(SerialDevice(path:path,name:name ?? "USB serial reader",vendor:vendor,product:product))
        }
        return result.sorted { $0.path < $1.path }
    }
}

/// Nonblocking 8N1 serial connection shared by magstripe and UHF reader transports.
@MainActor final class SerialChannel {
    private var fd: Int32 = -1
    private var original = termios()
    private var poll: Task<Void,Never>?
    var onBytes: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    let path: String
    let baud: Int
    var isOpen: Bool { fd >= 0 }
    init(path: String, baud: Int) throws {
        self.path = path; self.baud = baud
        guard [9600,19200,38400,57600,115200,230400].contains(baud), path.hasPrefix("/dev/cu.") || path.hasPrefix("/dev/ttys") else { throw StripeError("Choose a valid serial port and baud rate.") }
        fd = Darwin.open(path,O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { throw StripeError("Cannot open \(path): \(String(cString:strerror(errno))). Close other reader apps and try again.") }
        do {
            guard flock(fd,LOCK_EX | LOCK_NB) == 0, ioctl(fd,TIOCEXCL) == 0 else { throw StripeError("This serial reader is already in use.") }
            guard tcgetattr(fd,&original) == 0 else { throw StripeError("Cannot read serial port settings.") }
            var settings = original
            cfmakeraw(&settings)
            settings.c_cflag &= ~tcflag_t(CSIZE | PARENB | CSTOPB | CRTSCTS)
            settings.c_cflag |= tcflag_t(CS8 | CLOCAL | CREAD)
            settings.c_cflag &= ~tcflag_t(HUPCL)
            cfsetspeed(&settings,speed_t(baud))
            guard tcsetattr(fd,TCSANOW,&settings) == 0 else { throw StripeError("Cannot set the serial baud rate.") }
            var modem = Int32(TIOCM_DTR | TIOCM_RTS)
            _ = ioctl(fd,TIOCMBIS,&modem)
            tcflush(fd,TCIFLUSH)
        } catch { _ = Darwin.close(fd); fd = -1; throw error }
        poll = Task { [weak self] in
            while !Task.isCancelled {
                self?.readAvailable()
                guard self?.isOpen == true else { break }
                do { try await Task.sleep(nanoseconds:10_000_000) } catch { break }
            }
        }
    }
    private func readAvailable() {
        guard fd >= 0 else { return }
        var bytes = [UInt8](repeating:0,count:4096)
        for _ in 0..<16 {
            let count = Darwin.read(fd,&bytes,bytes.count)
            if count > 0 { onBytes?(Array(bytes.prefix(count))) }
            else if count < 0 && errno != EAGAIN && errno != EINTR { fail(StripeError("Serial reader disconnected: \(String(cString:strerror(errno))).")); return }
            else { return }
            if fd < 0 { return }
        }
    }
    func send(_ bytes: [UInt8]) throws {
        guard fd >= 0 else { throw StripeError("Serial reader is disconnected.") }
        guard !bytes.isEmpty else { return }
        let count = bytes.withUnsafeBytes { Darwin.write(fd,$0.baseAddress,bytes.count) }
        guard count == bytes.count else {
            let error = StripeError("Serial command could not be sent completely. Reconnect before trying again.")
            fail(error); throw error
        }
    }
    private func fail(_ error: Error) { let callback = onDisconnect; close(); callback?(error) }
    func close() {
        poll?.cancel(); poll = nil
        guard fd >= 0 else { return }
        _ = tcsetattr(fd,TCSANOW,&original); _ = ioctl(fd,TIOCNXCL); _ = Darwin.close(fd); fd = -1
        onBytes = nil; onDisconnect = nil
    }
    deinit { poll?.cancel(); if fd >= 0 { _ = ioctl(fd,TIOCNXCL); _ = Darwin.close(fd) } }
}

@MainActor final class SerialMSRTransport: MSRTransport {
    private let channel: SerialChannel
    private var assembler = MSRStreamAssembler(command:[])
    private var pending: CheckedContinuation<[UInt8],Error>?
    private var timer: Task<Void,Never>?
    var onDisconnect: (() -> Void)?
    init(path: String, baud: Int) throws {
        channel = try SerialChannel(path:path,baud:baud)
        channel.onBytes = { [weak self] bytes in
            guard let self, self.pending != nil else { return }
            do { if let reply = try self.assembler.accept(bytes) { self.finish(.success(reply)) } }
            catch { self.finish(.failure(error)) }
        }
        channel.onDisconnect = { [weak self] error in self?.finish(.failure(error)); self?.onDisconnect?() }
    }
    func send(_ message: [UInt8]) throws { try channel.send(message) }
    func exchange(_ message: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        guard pending == nil else { throw StripeError("Another serial operation is running.") }
        try Task.checkCancellation()
        assembler = MSRStreamAssembler(command:message)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                pending = continuation
                do { try channel.send(message) } catch { finish(.failure(error)); return }
                timer = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds:UInt64(timeout*1e9)) } catch { return }
                    self?.finish(.failure(StripeError("Serial writer timed out. Check the port, baud rate and swipe, then reconnect.")))
                }
            }
        } onCancel: { Task { @MainActor [weak self] in self?.cancel() } }
    }
    private func finish(_ result: Result<[UInt8],Error>) { timer?.cancel(); timer = nil; let c = pending; pending = nil; c?.resume(with:result) }
    func cancel() { finish(.failure(CancellationError())) }
    func close() { cancel(); channel.close() }
}
