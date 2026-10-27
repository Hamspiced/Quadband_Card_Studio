// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import CoreBluetooth
import Combine
import MagStripeCore

struct BluetoothWriter: Identifiable {
    let id: UUID
    let name: String
    let peripheral: CBPeripheral
}

@MainActor final class BluetoothController: NSObject, ObservableObject, CBCentralManagerDelegate {
    @Published var devices: [BluetoothWriter] = []
    @Published var scanning = false
    @Published var message = "Scan for your nearby MSR X6."
    private var central: CBCentralManager?
    private var channel: BluetoothTransport?
    private var wantsScan = false
    private var scanTimeout: Task<Void, Never>?

    func scan() {
        wantsScan = true
        if central == nil { central = CBCentralManager(delegate: self, queue: .main); return }
        updateState()
    }
    func stopScan() {
        wantsScan = false; scanning = false; central?.stopScan()
        scanTimeout?.cancel(); scanTimeout = nil
    }
    private func updateState() {
        guard let central else { return }
        switch central.state {
        case .poweredOn:
            guard wantsScan else { return }
            scanning = true; message = "Scanning for MSR X6…"
            central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
            // The observed X6 uses FF00; also include already-connected readers.
            for peripheral in central.retrieveConnectedPeripherals(withServices: ["FF00", "FFF0", "1800", "1801"].map(CBUUID.init(string:))) { discovered(peripheral, name: peripheral.name) }
            scanTimeout?.cancel()
            scanTimeout = Task { [weak self] in
                do { try await Task.sleep(nanoseconds: 12_000_000_000) } catch { return }
                guard let self else { return }
                self.stopScan()
                self.message = self.devices.isEmpty ? "No X6 found. Turn it on and disconnect other reader apps." : "Choose your reader, then Connect."
            }
        case .unauthorized: scanning = false; message = "Allow Quadband Card Studio in System Settings → Privacy & Security → Bluetooth."
        case .poweredOff: scanning = false; message = "Turn on Bluetooth in System Settings."
        case .unsupported: scanning = false; message = "Bluetooth is unavailable on this Mac."
        default: message = "Starting Bluetooth…"
        }
        if central.state != .poweredOn { channel?.disconnected("Bluetooth is no longer available.") }
    }
    private func discovered(_ peripheral: CBPeripheral, name: String?) {
        let name = name ?? peripheral.name ?? ""
        guard name.uppercased().contains("MSR") || name.uppercased().contains("X6") else { return }
        guard !devices.contains(where: { $0.id == peripheral.identifier }) else { return }
        devices.append(BluetoothWriter(id: peripheral.identifier, name: name, peripheral: peripheral))
        message = "Choose your reader, then Connect."
    }
    func connect(_ id: UUID) async throws -> BluetoothTransport {
        guard let central, central.state == .poweredOn, let device = devices.first(where: { $0.id == id }) else {
            throw StripeError("Scan and choose a Bluetooth MSR X6 first.")
        }
        stopScan()
        channel?.close(); channel = nil
        // A new connection must not inherit the old link's queued disconnect callback.
        let deadline = Date().addingTimeInterval(4)
        while device.peripheral.state == .disconnecting {
            try await Task.sleep(nanoseconds: 50_000_000)
            guard Date() < deadline else { throw StripeError("Bluetooth is still disconnecting. Try Connect again.") }
        }
        try Task.checkCancellation()
        let transport = BluetoothTransport(peripheral: device.peripheral, central: central)
        channel = transport
        do { try await transport.open(); return transport }
        catch { transport.close(); throw error }
    }
    func close() { stopScan(); channel?.close(); channel = nil }

    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated { updateState() }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated { discovered(peripheral, name: advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name) }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.didConnect() } }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.disconnected(error?.localizedDescription ?? "Bluetooth connection failed.") } }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.disconnected(error?.localizedDescription ?? "Bluetooth reader disconnected.") } }
    }
}

@MainActor final class BluetoothTransport: NSObject, MSRTransport, CBPeripheralDelegate {
    let peripheral: CBPeripheral
    private let central: CBCentralManager
    private var writer: CBCharacteristic?
    private var reader: CBCharacteristic?
    private var opening: CheckedContinuation<Void, Error>?
    private var pending: CheckedContinuation<[UInt8], Error>?
    private var deadline: Task<Void, Never>?
    private var assembler: MSRStreamAssembler?
    private var outgoing: [Data] = []
    private var awaitingWrite = false
    private var ready = false
    private var closing = false
    var onDisconnect: (() -> Void)?

    init(peripheral: CBPeripheral, central: CBCentralManager) {
        self.peripheral = peripheral; self.central = central
        super.init()
    }
    func open() async throws {
        try Task.checkCancellation()
        peripheral.delegate = self
        try await withTaskCancellationHandler(operation: {
            try await withCheckedThrowingContinuation { continuation in
                opening = continuation
                setDeadline(seconds: 15, message: "Bluetooth connection timed out. Check power and close other reader apps.")
                if peripheral.state == .connected { didConnect() }
                else { central.connect(peripheral) }
            }
        }, onCancel: { [weak self] in Task { @MainActor in self?.close() } })
    }
    func didConnect() { guard !closing else { return }; peripheral.discoverServices([CBUUID(string: "FF00")]) }
    func disconnected(_ message: String) {
        guard !closing else { return }
        fail(StripeError(message)); ready = false
        onDisconnect?()
    }
    func close() {
        guard !closing else { return }
        closing = true; ready = false
        fail(CancellationError())
        outgoing = []; awaitingWrite = false
        peripheral.delegate = nil
        if peripheral.state != .disconnected { central.cancelPeripheralConnection(peripheral) }
    }
    func cancel() {
        fail(CancellationError()); outgoing = []
        // If a BLE write is outstanding, closing the link is more reliable than queueing RESET.
        if ready && !awaitingWrite { try? send(MSRProtocol.reset) }
    }
    func send(_ message: [UInt8]) throws {
        guard ready, let writer, peripheral.state == .connected else { throw StripeError("The Bluetooth writer is not connected.") }
        let type: CBCharacteristicWriteType = writer.properties.contains(.write) ? .withResponse : .withoutResponse
        let size = min(20, peripheral.maximumWriteValueLength(for: type))
        guard size > 0 else { throw StripeError("Bluetooth packet size is unavailable.") }
        outgoing += stride(from: 0, to: message.count, by: size).map { Data(message[$0..<min($0 + size, message.count)]) }
        pump()
    }
    private func pump() {
        guard ready, !awaitingWrite, let writer, !outgoing.isEmpty else { return }
        if writer.properties.contains(.write) {
            awaitingWrite = true
            peripheral.writeValue(outgoing.removeFirst(), for: writer, type: .withResponse)
        } else {
            while peripheral.canSendWriteWithoutResponse && !outgoing.isEmpty {
                peripheral.writeValue(outgoing.removeFirst(), for: writer, type: .withoutResponse)
            }
        }
    }
    func exchange(_ message: [UInt8], timeout: TimeInterval) async throws -> [UInt8] {
        try Task.checkCancellation()
        guard pending == nil else { throw StripeError("A Bluetooth operation is already running.") }
        assembler = MSRStreamAssembler(command: message)
        return try await withTaskCancellationHandler(operation: {
            try await withCheckedThrowingContinuation { continuation in
                pending = continuation
                setDeadline(seconds: timeout, message: "Bluetooth response timed out. Reconnect and try again.")
                do { try send(message) } catch { fail(error) }
            }
        }, onCancel: { [weak self] in Task { @MainActor in self?.cancel() } })
    }
    private func setDeadline(seconds: TimeInterval, message: String) {
        deadline?.cancel()
        deadline = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000)) } catch { return }
            self?.fail(StripeError(message))
        }
    }
    private func fail(_ error: Error) {
        deadline?.cancel(); deadline = nil
        let start = opening; opening = nil; start?.resume(throwing: error)
        let response = pending; pending = nil; response?.resume(throwing: error)
        assembler = nil
    }
    private func servicesFound(_ error: Error?) {
        if let error { fail(error); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == CBUUID(string: "FF00") }) else {
            fail(StripeError("This Bluetooth revision does not expose the supported X6 data service (FF00).")); return
        }
        peripheral.discoverCharacteristics([CBUUID(string: "FF01"), CBUUID(string: "FF02")], for: service)
    }
    private func characteristicsFound(_ service: CBService, error: Error?) {
        if let error { fail(error); return }
        writer = service.characteristics?.first { $0.uuid == CBUUID(string: "FF02") && ($0.properties.contains(.write) || $0.properties.contains(.writeWithoutResponse)) }
        reader = service.characteristics?.first { $0.uuid == CBUUID(string: "FF01") && ($0.properties.contains(.notify) || $0.properties.contains(.indicate)) }
        guard writer != nil, let reader else { fail(StripeError("The X6 Bluetooth read/write characteristics are unavailable.")); return }
        peripheral.setNotifyValue(true, for: reader)
    }
    private func notificationReady(_ characteristic: CBCharacteristic, error: Error?) {
        if let error { fail(error); return }
        guard characteristic === reader, characteristic.isNotifying else { return }
        ready = true; deadline?.cancel(); deadline = nil
        let continuation = opening; opening = nil; continuation?.resume()
    }
    private func received(_ characteristic: CBCharacteristic, error: Error?) {
        if let error { fail(error); return }
        guard characteristic === reader, pending != nil, let data = characteristic.value else { return }
        do {
            if let bytes = try assembler?.accept(Array(data)) {
                deadline?.cancel(); deadline = nil
                let continuation = pending; pending = nil; assembler = nil
                continuation?.resume(returning: bytes)
            }
        } catch { fail(error) }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated { servicesFound(error) }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        MainActor.assumeIsolated { characteristicsFound(service, error: error) }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { notificationReady(characteristic, error: error) }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { received(characteristic, error: error) }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            awaitingWrite = false
            if let error { outgoing = []; fail(error) } else { pump() }
        }
    }
    nonisolated func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        MainActor.assumeIsolated { pump() }
    }
}
