// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import CoreBluetooth
import Combine
import MagStripeCore

private let nusService = CBUUID(string:"6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
private let nusRX = CBUUID(string:"6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
private let batteryService = CBUUID(string:"180F")
private let batteryLevel = CBUUID(string:"2A19")
private let nusTX = CBUUID(string:"6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

struct ChameleonBluetoothDevice: Identifiable {
    let peripheral: CBPeripheral
    let name: String
    var id: UUID { peripheral.identifier }
}
@MainActor final class ChameleonBluetooth: NSObject, ObservableObject, CBCentralManagerDelegate {
    @Published var devices: [ChameleonBluetoothDevice] = []
    @Published var scanning = false
    @Published var message = "Wake the Chameleon, close other Chameleon apps, then scan."
    private var central: CBCentralManager?
    private var channel: ChameleonBLEChannel?
    private var wantsScan = false
    private var timer: Task<Void,Never>?
    func scan() {
        wantsScan = true
        if central == nil { central = CBCentralManager(delegate:self,queue:.main) } else { updateState() }
    }
    func stop() { wantsScan = false; scanning = false; central?.stopScan(); timer?.cancel(); timer = nil }
    private func updateState() {
        guard let central else { return }
        switch central.state {
        case .poweredOn:
            guard wantsScan else { return }
            scanning = true; message = "Scanning for Chameleon Ultra…"
            central.scanForPeripherals(withServices:nil,options:[CBCentralManagerScanOptionAllowDuplicatesKey:false])
            for p in central.retrieveConnectedPeripherals(withServices:[nusService]) { found(p,name:p.name) }
            timer?.cancel(); timer = Task { [weak self] in
                do { try await Task.sleep(nanoseconds:12_000_000_000) } catch { return }
                guard let self else { return }; self.stop()
                self.message = self.devices.isEmpty ? "No Chameleon found. Wake it and disconnect other reader apps." : "Select your Chameleon and connect."
            }
        case .unauthorized: stop(); message = "Allow Quadband Card Studio in System Settings → Privacy & Security → Bluetooth."
        case .poweredOff: stop(); message = "Turn on Bluetooth in System Settings."
        case .unsupported: stop(); message = "Bluetooth is unavailable on this Mac."
        default: message = "Starting Bluetooth…"
        }
        if central.state != .poweredOn { channel?.lost(StripeError(message)) }
    }
    private func found(_ peripheral: CBPeripheral, name: String?) {
        guard !devices.contains(where: { $0.id == peripheral.identifier }) else { return }
        devices.append(ChameleonBluetoothDevice(peripheral:peripheral,name:name ?? peripheral.name ?? "UART device · \(peripheral.identifier.uuidString.prefix(8))"))
    }
    func connect(_ id: UUID) async throws -> ChameleonBLEChannel {
        guard let central, central.state == .poweredOn, let device = devices.first(where: { $0.id == id }) else { throw StripeError("Scan and select your Bluetooth Chameleon first.") }
        stop(); channel?.close(); channel = nil
        let deadline = Date().addingTimeInterval(4)
        while device.peripheral.state == .disconnecting {
            try await Task.sleep(nanoseconds:50_000_000)
            guard Date() < deadline else { throw StripeError("Bluetooth is still disconnecting. Try again shortly.") }
        }
        try Task.checkCancellation()
        let value = ChameleonBLEChannel(peripheral:device.peripheral,central:central); channel = value
        value.onProgress = { [weak self] text in self?.message = text }
        do { try await value.open(); return value } catch { value.close(); throw error }
    }
    func close() { stop(); channel?.close(); channel = nil }
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) { MainActor.assumeIsolated { updateState() } }
    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String:Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated {
            let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? ""
            let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
            if name.localizedCaseInsensitiveContains("chameleon") || services.contains(nusService) { found(peripheral,name:name) }
        }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.connected() } }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.lost(error ?? StripeError("Bluetooth connection failed.")) } }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.lost(error ?? StripeError("Chameleon Bluetooth disconnected.")) } }
    }
}

@MainActor final class ChameleonBLEChannel: NSObject, ChameleonByteChannel, CBPeripheralDelegate {
    let peripheral: CBPeripheral
    private let central: CBCentralManager
    var onBytes: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    private var writer: CBCharacteristic?
    private var reader: CBCharacteristic?
    private var battery: CBCharacteristic?
    private var setup = ChameleonBLESetup()
    var onProgress: ((String) -> Void)?
    private var opening: CheckedContinuation<Void,Error>?
    private var timer: Task<Void,Never>?
    private var ready = false
    private var closed = false
    private var outgoing: [Data] = []
    private var awaitingWrite = false
    init(peripheral: CBPeripheral, central: CBCentralManager) { self.peripheral = peripheral; self.central = central; super.init() }
    func open() async throws {
        try Task.checkCancellation(); peripheral.delegate = self
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                opening = continuation
                timer = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds:90_000_000_000) } catch { return }
                    self?.lost(StripeError("Bluetooth connection timed out. Keep the Chameleon awake and complete macOS pairing if prompted. Cancel and reconnect if no prompt appears."))
                }
                if peripheral.state == .connected { connected() } else { central.connect(peripheral) }
            }
        } onCancel: { Task { @MainActor [weak self] in self?.close() } }
    }
    func connected() { guard !closed else { return }; onProgress?("Checking Chameleon Bluetooth services…"); peripheral.discoverServices([nusService,batteryService]) }
    func lost(_ error: Error) {
        guard !closed else { return }
        let native = error as NSError
        let reported: Error
        if native.domain == CBATTErrorDomain && [5,15].contains(native.code) {
            reported = StripeError("Bluetooth pairing is required. macOS did not establish an encrypted connection. Reconnect and complete its PIN prompt if shown. If no prompt appears, use USB to check the Chameleon pairing configuration. No security settings were changed.")
        } else { reported = error }
        let callback = onDisconnect; finishOpening(reported); close(); callback?(reported)
    }
    private func finishOpening(_ error: Error? = nil) {
        timer?.cancel(); timer = nil; let waiting = opening; opening = nil
        if let error { waiting?.resume(throwing:error) } else { waiting?.resume() }
    }
    func close() {
        guard !closed else { return }; closed = true; ready = false
        setup.cancel(); finishOpening(CancellationError()); outgoing = []; awaitingWrite = false; peripheral.delegate = nil
        if peripheral.state != .disconnected { central.cancelPeripheralConnection(peripheral) }
    }
    func send(_ bytes: [UInt8]) throws {
        guard ready, !closed, let writer, peripheral.state == .connected else { throw StripeError("Bluetooth Chameleon is disconnected.") }
        let type: CBCharacteristicWriteType = writer.properties.contains(.write) ? .withResponse : .withoutResponse
        // Conservative chunks also work with firmware negotiating the minimum ATT MTU.
        let size = min(20,peripheral.maximumWriteValueLength(for:type))
        guard size > 0 else { throw StripeError("Bluetooth transmission is still in progress.") }
        outgoing += stride(from:0,to:bytes.count,by:size).map { Data(bytes[$0..<min($0+size,bytes.count)]) }; pump()
    }
    private func pump() {
        guard ready, !closed, !awaitingWrite, let writer, !outgoing.isEmpty else { return }
        if writer.properties.contains(.write) { awaitingWrite = true; peripheral.writeValue(outgoing.removeFirst(),for:writer,type:.withResponse) }
        else { while peripheral.canSendWriteWithoutResponse && !outgoing.isEmpty { peripheral.writeValue(outgoing.removeFirst(),for:writer,type:.withoutResponse) } }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            guard let service = peripheral.services?.first(where: { $0.uuid == nusService }) else { lost(StripeError("This device does not provide the Chameleon Bluetooth UART service.")); return }
            peripheral.discoverCharacteristics([nusRX,nusTX],for:service)
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            guard !closed else { return }
            if service.uuid == batteryService {
                battery = service.characteristics?.first { $0.uuid == batteryLevel && $0.properties.contains(.read) }
                performSetup(setup.batteryDiscovered(readable:battery != nil))
                return
            }
            writer = service.characteristics?.first { $0.uuid == nusRX && ($0.properties.contains(.write) || $0.properties.contains(.writeWithoutResponse)) }
            reader = service.characteristics?.first { $0.uuid == nusTX && $0.properties.contains(.notify) }
            guard writer != nil, reader != nil else { lost(StripeError("Chameleon Bluetooth data channels are unavailable.")); return }
            performSetup(setup.uartDiscovered(hasBatteryService:peripheral.services?.contains(where: { $0.uuid == batteryService }) == true))
        }
    }
    private func performSetup(_ action: ChameleonBLESetup.Action) {
        guard !closed else { return }
        switch action {
        case .discoverBattery:
            if let service = peripheral.services?.first(where: { $0.uuid == batteryService }) { peripheral.discoverCharacteristics([batteryLevel],for:service) }
        case .readBattery:
            // The firmware protects this read with the same security level as NUS.
            // A GATT read gives macOS a pairing opportunity before subscribing to the UART.
            onProgress?("Checking Bluetooth security. Complete the macOS PIN prompt if one appears.")
            if let battery { peripheral.readValue(for:battery) }
        case .subscribe:
            if let reader { peripheral.setNotifyValue(true,for:reader) }
        case .ready:
            ready = true; onProgress?("Chameleon Bluetooth channel is ready."); finishOpening()
        case .none: break
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            if characteristic === reader && characteristic.isNotifying { performSetup(setup.subscribed()) }
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            if characteristic === battery {
                performSetup(setup.batteryRead()); return
            }
            if characteristic === reader, !closed, let bytes = characteristic.value { onBytes?(Array(bytes)) }
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { awaitingWrite = false; if let error { lost(error) } else { pump() } }
    }
    nonisolated func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) { MainActor.assumeIsolated { pump() } }
}

/// Only read-only GATT setup is allowed before the protocol client becomes ready.
struct ChameleonBLESetup {
    enum Action: Equatable { case none, discoverBattery, readBattery, subscribe, ready }
    private enum Phase { case uart, batteryDiscovery, batteryRead, subscribing, ready, closed }
    private var phase = Phase.uart
    mutating func uartDiscovered(hasBatteryService: Bool) -> Action {
        guard phase == .uart else { return .none }
        phase = hasBatteryService ? .batteryDiscovery : .subscribing
        return hasBatteryService ? .discoverBattery : .subscribe
    }
    mutating func batteryDiscovered(readable: Bool) -> Action {
        guard phase == .batteryDiscovery else { return .none }
        phase = readable ? .batteryRead : .subscribing
        return readable ? .readBattery : .subscribe
    }
    mutating func batteryRead() -> Action {
        guard phase == .batteryRead else { return .none }; phase = .subscribing; return .subscribe
    }
    mutating func subscribed() -> Action {
        guard phase == .subscribing else { return .none }; phase = .ready; return .ready
    }
    mutating func cancel() { phase = .closed }
}
