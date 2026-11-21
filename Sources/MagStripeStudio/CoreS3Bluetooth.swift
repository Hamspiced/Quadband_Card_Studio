import Foundation
import CoreBluetooth
import Combine
import MagStripeCore

struct CoreS3Device: Identifiable {
    let peripheral: CBPeripheral
    let name: String
    var id: UUID { peripheral.identifier }
}

@MainActor final class CoreS3Hub: NSObject, ObservableObject, CBCentralManagerDelegate {
    @Published var devices: [CoreS3Device] = []
    @Published var selected: UUID?
    @Published var scanning = false
    @Published var connecting = false
    @Published var capabilities: CoreS3Capabilities?
    @Published var message = "Open pairing on the CoreS3 screen, then scan."
    @Published var password = ""
    @Published var pack = ""
    @Published var usePassword = false
    private var central: CBCentralManager?
    private var channel: CoreS3BLEChannel?
    private var scanTimer: Task<Void,Never>?
    private var openingTask: Task<Void,Never>?
    private(set) var session: CoreS3Session?
    var onChange: ((CoreS3Capabilities?) -> Void)?
    var prepareDisconnect: (() async -> Void)?
    var hfReaderName: String? {
        guard capabilities?.remoteAllowed == true,
              let slot = capabilities?.peripherals.first(where: { $0.channel == 1 && $0.present && $0.capabilities.contains("identify") }) else { return nil }
        return "CoreS3 · \(slot.kind.uppercased()) · generation \(slot.generation)"
    }
    func refreshPeripherals() {
        guard let session, !connecting else { return }
        Task {
            do { _ = try await session.refreshCapabilities(); message = "Detected CoreS3 modules refreshed." }
            catch { message = error.localizedDescription }
        }
    }
    func cancelConnection() { openingTask?.cancel(); channel?.close(); session?.close() }
    func attach(_ shared: CoreS3Session) {
        session = shared; capabilities = shared.capabilities
        shared.onCapabilities = { [weak self] value in
            guard let self else { return }
            if !value.remoteAllowed || self.capabilities?.peripherals.first(where: { $0.channel == 1 }) != value.peripherals.first(where: { $0.channel == 1 }) {
                self.password = ""; self.pack = ""; self.usePassword = false
            }
            self.capabilities = value; self.onChange?(value)
        }
        shared.onDisconnect = { [weak self] error in
            self?.session = nil; self?.capabilities = nil; self?.password = ""; self?.pack = ""; self?.usePassword = false
            self?.message = error.localizedDescription; self?.onChange?(nil)
        }
        onChange?(capabilities)
    }
    var supportsAuthentication: Bool { capabilities?.remoteAllowed == true && capabilities?.peripherals.first { $0.channel == 1 }?.capabilities.contains("ntag_auth") == true }
    func scan() {
        guard !connecting, session == nil else { return }
        scanning = true
        if central == nil { central = CBCentralManager(delegate:self,queue:.main) } else { updateState() }
    }
    func stopScan() { central?.stopScan(); scanning = false; scanTimer?.cancel(); scanTimer = nil }
    private func updateState() {
        guard let central else { return }
        if central.state == .poweredOn {
            guard scanning else { return }
            message = "Scanning for Quadband CoreS3…"
            central.scanForPeripherals(withServices:[CBUUID(string:CoreS3Protocol.service)])
            scanTimer?.cancel(); scanTimer = Task { [weak self] in
                do { try await Task.sleep(nanoseconds:12_000_000_000) } catch { return }
                self?.stopScan()
            }
        } else {
            let error = StripeError(central.state == .unauthorized ? "Allow Bluetooth access in System Settings → Privacy & Security." : "Bluetooth is unavailable or powered off.")
            if central.state != .unknown && central.state != .resetting { stopScan(); channel?.lost(error); message = error.localizedDescription }
        }
    }
    func connect() {
        guard !connecting, session == nil, let central, central.state == .poweredOn,
              let device = devices.first(where: { $0.id == selected }) else { return }
        stopScan(); connecting = true; password = ""; pack = ""; usePassword = false
        let deadline = ProcessInfo.processInfo.systemUptime + 90
        openingTask = Task { [self] in
            var value: CoreS3BLEChannel?
            defer { connecting = false; openingTask = nil }
            do {
                while device.peripheral.state == .disconnecting {
                    try await Task.sleep(nanoseconds:50_000_000)
                    guard (deadline - ProcessInfo.processInfo.systemUptime) > 0 else { throw StripeError("The prior CoreS3 link is still disconnecting.") }
                }
                try Task.checkCancellation()
                let link = CoreS3BLEChannel(peripheral:device.peripheral,central:central); value = link; channel = link
                link.onProgress = { [weak self] in self?.message = $0 }
                try await link.open(timeout:max(0.01,(deadline - ProcessInfo.processInfo.systemUptime)))
                try Task.checkCancellation()
                let shared = CoreS3Session(channel:link); attach(shared)
                guard (deadline - ProcessInfo.processInfo.systemUptime) > 0 else { throw StripeError("CoreS3 pairing setup timed out.") }
                _ = try await shared.hello(timeout:min(90,(deadline - ProcessInfo.processInfo.systemUptime)))
                message = capabilities?.modeDescription ?? "Paired CoreS3 session ready."
            } catch { value?.close(); session = nil; capabilities = nil; message = error is CancellationError ? "CoreS3 connection cancelled." : error.localizedDescription }
        }
    }
    func disconnect() {
        stopScan()
        if let prepareDisconnect, session != nil {
            connecting = true
            Task { await prepareDisconnect(); close(); connecting = false }
        } else { close() }
    }
    private func close() { session?.close(); session = nil; channel?.close(); channel = nil; capabilities = nil; password = ""; pack = ""; usePassword = false; onChange?(nil) }
    func withHF<T>(_ reader: String, _ work: (CoreS3HFClient) async throws -> T) async throws -> T {
        guard reader == hfReaderName, let session else { throw StripeError("CoreS3 HF reader changed. Refresh and explicitly select it again.") }
        let authentication: NTAGAuthentication
        if usePassword {
            guard supportsAuthentication else { throw StripeError("This module does not advertise NTAG password authentication.") }
            let bytes = try HexBytes.parse(password,count:4)
            authentication = pack.isEmpty ? .password(bytes) : .passwordAndPACK(bytes,try HexBytes.parse(pack,count:2))
        } else { authentication = .none }
        let lease = try session.acquire(1,capability:"identify"); defer { session.release(lease) }
        return try await work(CoreS3HFClient(session:session,lease:lease,authentication:authentication))
    }
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) { MainActor.assumeIsolated { updateState() } }
    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String:Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated {
            guard !devices.contains(where: { $0.id == peripheral.identifier }) else { return }
            devices.append(CoreS3Device(peripheral:peripheral,name:advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "Quadband CoreS3"))
            if selected == nil { selected = peripheral.identifier }
        }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.connected() } }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.lost(error ?? StripeError("CoreS3 Bluetooth connection failed.")) } }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { if channel?.peripheral === peripheral { channel?.lost(error ?? StripeError("CoreS3 Bluetooth disconnected.")) } }
    }
}

@MainActor final class CoreS3BLEChannel: NSObject, CoreS3ByteChannel, CBPeripheralDelegate {
    let peripheral: CBPeripheral
    private let central: CBCentralManager
    private var writer: CBCharacteristic?
    private var reader: CBCharacteristic?
    private var opening: CheckedContinuation<Void,Error>?
    private var timer: Task<Void,Never>?
    private var outgoing: [Data] = []
    private var awaitingWrite = false
    private var ready = false
    private var closed = false
    var onBytes: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    var onProgress: ((String) -> Void)?
    init(peripheral: CBPeripheral, central: CBCentralManager) { self.peripheral = peripheral; self.central = central; super.init() }
    func open(timeout: TimeInterval = 90) async throws {
        try Task.checkCancellation(); peripheral.delegate = self
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                opening = continuation
                timer = Task { [weak self] in
                    do { try await Task.sleep(nanoseconds:UInt64(timeout * 1e9)) } catch { return }
                    self?.lost(StripeError("CoreS3 pairing timed out. Authorize locally on its screen and complete the macOS passkey prompt."))
                }
                if peripheral.state == .connected { connected() } else { central.connect(peripheral) }
            }
        } onCancel: { Task { @MainActor [weak self] in self?.close() } }
    }
    func connected() { guard !closed else { return }; onProgress?("Discovering dedicated CoreS3 service…"); peripheral.discoverServices([CBUUID(string:CoreS3Protocol.service)]) }
    func lost(_ error: Error) {
        guard !closed else { return }
        let callback = onDisconnect; finishOpening(error); close(); callback?(error)
    }
    private func finishOpening(_ error: Error? = nil) {
        timer?.cancel(); timer = nil; let waiting = opening; opening = nil
        if let error { waiting?.resume(throwing:error) } else { waiting?.resume() }
    }
    func close() {
        guard !closed else { return }; closed = true; ready = false; outgoing = []; awaitingWrite = false
        finishOpening(CancellationError()); peripheral.delegate = nil
        if peripheral.state != .disconnected { central.cancelPeripheralConnection(peripheral) }
    }
    func send(_ bytes: [UInt8]) throws {
        guard ready, !closed, peripheral.state == .connected, outgoing.reduce(0, { $0 + $1.count }) + bytes.count <= 4156 else { throw StripeError("CoreS3 BLE channel is busy or disconnected.") }
        let size = min(20,peripheral.maximumWriteValueLength(for:.withResponse))
        guard size > 0, bytes.count <= 2078 else { throw StripeError("Invalid CoreS3 BLE transmission.") }
        outgoing += stride(from:0,to:bytes.count,by:size).map { Data(bytes[$0..<min($0+size,bytes.count)]) }; pump()
    }
    private func pump() {
        guard ready, !closed, !awaitingWrite, let writer, !outgoing.isEmpty else { return }
        awaitingWrite = true; peripheral.writeValue(outgoing.removeFirst(),for:writer,type:.withResponse)
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            guard let service = peripheral.services?.first(where: { $0.uuid == CBUUID(string:CoreS3Protocol.service) }) else { lost(StripeError("Dedicated CoreS3 service missing. No UART fallback is permitted.")); return }
            peripheral.discoverCharacteristics([CBUUID(string:CoreS3Protocol.rx),CBUUID(string:CoreS3Protocol.tx)],for:service)
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            writer = service.characteristics?.first { $0.uuid == CBUUID(string:CoreS3Protocol.rx) && $0.properties.contains(.write) }
            reader = service.characteristics?.first { $0.uuid == CBUUID(string:CoreS3Protocol.tx) && $0.properties.contains(.indicate) }
            guard writer != nil, let reader else { lost(StripeError("CoreS3 authenticated write/indication characteristics are missing.")); return }
            onProgress?("Authorize pairing on the CoreS3 and confirm the macOS passkey prompt.")
            peripheral.setNotifyValue(true,for:reader)
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            guard characteristic === reader, characteristic.isNotifying, !closed else { lost(StripeError("CoreS3 indication subscription failed.")); return }
            ready = true; finishOpening()
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            if let error { lost(error); return }
            if characteristic === reader, ready, !closed, let bytes = characteristic.value { onBytes?(Array(bytes)) }
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { awaitingWrite = false; if let error { lost(error) } else { pump() } }
    }
}
