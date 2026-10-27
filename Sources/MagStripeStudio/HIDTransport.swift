// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import IOKit
import IOKit.hid
import MagStripeCore

struct WriterDevice: Identifiable {
    let id: UInt64
    let name: String
    let vendor: Int
    let product: Int
    let device: IOHIDDevice
    var detail: String { String(format: "USB %04X:%04X", vendor, product) }
}

@MainActor final class WriterDiscovery {
    private let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
    func scan() -> [WriterDevice] {
        // Enumerating does not open or capture keyboards. Only the selected writer is opened.
        IOHIDManagerSetDeviceMatching(manager, nil)
        guard let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else { return [] }
        return devices.compactMap { device -> WriterDevice? in
            let name = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "USB device"
            let vendor = (IOHIDDeviceGetProperty(device, kIOHIDVendorIDKey as CFString) as? NSNumber)?.intValue ?? 0
            let product = (IOHIDDeviceGetProperty(device, kIOHIDProductIDKey as CFString) as? NSNumber)?.intValue ?? 0
            let page = (IOHIDDeviceGetProperty(device, kIOHIDPrimaryUsagePageKey as CFString) as? NSNumber)?.intValue ?? 0
            let usage = (IOHIDDeviceGetProperty(device, kIOHIDPrimaryUsageKey as CFString) as? NSNumber)?.intValue ?? 0
            guard !(page == 1 && usage == 6), vendor == 0x0801 || name.lowercased().contains("msr") || name.lowercased().contains("magnetic") else { return nil }
            var registryID: UInt64 = 0
            IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(device), &registryID)
            return WriterDevice(id: registryID, name: name, vendor: vendor, product: product, device: device)
        }.sorted { $0.name < $1.name }
    }
}

@MainActor final class HIDTransport: MSRTransport {
    private let device: IOHIDDevice
    private let inputBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 4096)
    private var assembler = HIDAssembler()
    private var pending: CheckedContinuation<[UInt8], Error>?
    private var timeoutTask: Task<Void, Never>?
    private var opened = false
    var onDisconnect: (() -> Void)?

    init(device: IOHIDDevice) throws {
        self.device = device
        inputBuffer.initialize(repeating: 0, count: 4096)
        let status = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
        guard status == kIOReturnSuccess else {
            throw StripeError(String(format: "Could not open the USB writer (0x%08X). Close the supplied MSRX utility and reconnect the cable. If macOS requests device access, allow it.", status))
        }
        opened = true
        let context = Unmanaged.passUnretained(self).toOpaque()
        IOHIDDeviceRegisterInputReportCallback(device, inputBuffer, 4096, { context, status, _, _, _, report, length in
            guard let context else { return }
            let transport = Unmanaged<HIDTransport>.fromOpaque(context).takeUnretainedValue()
            let bytes = Array(UnsafeBufferPointer(start: report, count: length))
            // The device is scheduled on the main run loop.
            MainActor.assumeIsolated { transport.received(bytes, status: status) }
        }, context)
        IOHIDDeviceRegisterRemovalCallback(device, { context, _, _ in
            guard let context else { return }
            let transport = Unmanaged<HIDTransport>.fromOpaque(context).takeUnretainedValue()
            MainActor.assumeIsolated {
                transport.finish(.failure(StripeError("The USB writer was disconnected.")))
                transport.onDisconnect?()
            }
        }, context)
        IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
    }

    func close() {
        guard opened else { return }
        finish(.failure(CancellationError()))
        IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        IOHIDDeviceRegisterInputReportCallback(device, inputBuffer, 4096, nil, nil)
        IOHIDDeviceRegisterRemovalCallback(device, nil, nil)
        IOHIDDeviceClose(device, IOOptionBits(kIOHIDOptionsTypeNone))
        opened = false
    }
    deinit {
        // The owner explicitly closes before releasing; no C callback retains self.
        inputBuffer.deallocate()
    }
    func send(_ message: [UInt8]) throws {
        guard opened else { throw StripeError("Connect the USB writer first.") }
        for packet in HIDFraming.packets(message) {
            let status = packet.withUnsafeBufferPointer {
                IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, 0, $0.baseAddress!, packet.count)
            }
            guard status == kIOReturnSuccess else {
                throw StripeError(String(format: "USB command failed (0x%08X). Reconnect the writer and close any other MSR software.", status))
            }
        }
    }
    func exchange(_ message: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        try Task.checkCancellation()
        guard pending == nil else { throw StripeError("A device command is already in progress.") }
        assembler.reset()
        return try await withTaskCancellationHandler(operation: {
            try await withCheckedThrowingContinuation { continuation in
                pending = continuation
                timeoutTask = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000)) }
                    catch { return }
                    self?.finish(.failure(StripeError("The device timed out. Reconnect and try again; swipe within 45 seconds when prompted.")))
                }
                do { try send(message) }
                catch { finish(.failure(error)) }
            }
        }, onCancel: { [weak self] in
            Task { @MainActor in self?.cancel() }
        })
    }
    func cancel() {
        finish(.failure(CancellationError()))
        try? send(MSRProtocol.reset)
        assembler.reset()
    }
    private func received(_ bytes: [UInt8], status: IOReturn) {
        guard pending != nil else { return }
        guard status == kIOReturnSuccess else { finish(.failure(StripeError("USB read failed."))); return }
        do {
            if let message = try assembler.accept(bytes) { finish(.success(message)) }
        } catch { finish(.failure(error)) }
    }
    private func finish(_ result: Result<[UInt8], Error>) {
        timeoutTask?.cancel(); timeoutTask = nil
        let continuation = pending
        pending = nil
        continuation?.resume(with: result)
    }
}
