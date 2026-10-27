// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import AppKit
import SwiftUI
import Combine
import MagStripeCore

@MainActor final class AppModel: ObservableObject {
    @Published var devices: [WriterDevice] = []
    @Published var selectedDevice: UInt64 = 0
    @Published var connected = false
    @Published var busy = false
    @Published var status = "Connect your MSR X6 or choose the MSR600 reader."
    @Published var statusKind = StatusKind.neutral
    @Published var drafts = CardData.shortTest.tracks
    @Published var densities = [210, 75, 210]
    @Published var highCoercivity = true
    @Published var readCard: CardData?
    @Published var expectedCard: CardData?
    @Published var verification: [Bool?] = []
    @Published var log: [String] = []
    @Published var readerMode = ReaderMode.keyboard
    @Published var keyboardArmed = false
    @Published var captureGeneration = 0
    @Published var selectedPage = AppPage.write
    @Published var receivedAt: Date?
    @Published var rawSwipe = ""
    @Published var inputFormat = SwipeInputFormat.automatic
    let bluetooth = BluetoothController()
    let nfc: NFCModel
    let uhf = UHFModel()
    let lf = LFModel()
    @Published var serialPorts: [SerialDevice] = []
    @Published var selectedSerial = ""
    @Published var serialBaud = 9600
    @Published var connectionMode = ConnectionMode.usb
    @Published var selectedBluetooth: UUID?
    enum ConnectionMode: String, CaseIterable { case usb = "USB", bluetooth = "Bluetooth", serial = "Serial" }
    private let discovery = WriterDiscovery()
    private var transport: (any MSRTransport)?
    private var client: MSRClient?
    private var operation: Task<Void, Never>?
    private var operationGeneration = 0
    private var writeAcknowledged = false
    private var hfBusyObservation: AnyCancellable?

    enum StatusKind { case neutral, working, success, error }
    enum ReaderMode: String, CaseIterable { case keyboard = "MSR600 · Keyboard", writer = "MSR X6 / MSR605" }
    enum AppPage: String, CaseIterable { case write = "Magstripe Write", read = "Magstripe Read", nfc = "HF Workspace", uhf = "UHF Workspace", lf = "LF Workspace", help = "Getting started" }
    var settings: EncodingSettings { EncodingSettings(densities: densities, highCoercivity: highCoercivity) }
    var validationError: String? {
        do { try CardData(drafts).validate(densities: densities); return nil }
        catch { return error.localizedDescription }
    }
    init(scanDevices: Bool = true) { nfc = NFCModel(service:WorkspaceNFCService(lf:lf)); hfBusyObservation = nfc.$busy.sink { [weak lf] value in lf?.hfWorkflowBusy = value }; nfc.autoDiscovery = scanDevices; uhf.autoDiscovery = scanDevices; lf.autoDiscovery = scanDevices; if scanDevices { refresh() } }
    func refresh() {
        guard !busy else { return }
        devices = discovery.scan()
        serialPorts = SerialDevice.scan()
        if !serialPorts.contains(where: { $0.path == selectedSerial }) { selectedSerial = "" }
        if !devices.contains(where: { $0.id == selectedDevice }) { selectedDevice = devices.first?.id ?? 0 }
    }
    func note(_ message: String, kind: StatusKind = .neutral) {
        status = message; statusKind = kind
        let time = Date().formatted(date: .omitted, time: .standard)
        log.insert("\(time)  \(message)", at: 0)
        log = Array(log.prefix(60))
    }
    func connect() {
        guard !busy else { return }
        if connectionMode == .bluetooth { connectBluetooth(); return }
        if connectionMode == .serial { connectSerial(); return }
        guard let device = devices.first(where: { $0.id == selectedDevice }) else {
            note("No USB writer found. Connect the MSR X6 with a data cable, then Refresh.", kind: .error); return
        }
        disconnect(silent: true)
        operationGeneration += 1
        let generation = operationGeneration
        busy = true
        note("Connecting to \(device.name)…", kind: .working)
        operation = Task { [self] in
            do {
                let channel = try HIDTransport(device: device.device)
                transport = channel
                channel.onDisconnect = { [weak self] in
                    self?.disconnect(silent: true)
                    self?.note("USB writer disconnected. Reconnect and refresh.", kind: .error)
                }
                let session = MSRClient(transport: channel)
                client = session
                try await session.handshake()
                try Task.checkCancellation()
                connected = true
                note("MSR writer connected and responding.", kind: .success)
            } catch { if generation == operationGeneration { failed(error) } }
            if generation == operationGeneration { busy = false; operation = nil }
        }
    }
    private func connectSerial() {
        guard serialPorts.contains(where: { $0.path == selectedSerial }) else { note("Choose the MSR605 USB serial port first.",kind:.error); return }
        disconnect(silent:true)
        operationGeneration += 1; let generation = operationGeneration
        let path = selectedSerial, baud = serialBaud
        busy = true; note("Connecting to MSR605 at \(baud) baud…",kind:.working)
        operation = Task { [self] in
            do {
                let channel = try SerialMSRTransport(path:path,baud:baud); transport = channel
                channel.onDisconnect = { [weak self] in self?.disconnect(silent:true); self?.note("Serial writer disconnected. Reconnect and refresh.",kind:.error) }
                let session = MSRClient(transport:channel); client = session
                try await session.handshake(); try Task.checkCancellation()
                connected = true; note("MSR605-compatible serial writer connected and responding.",kind:.success)
            } catch { if generation == operationGeneration { failed(error) } }
            if generation == operationGeneration { busy = false; operation = nil }
        }
    }
    private func connectBluetooth() {
        guard let id = selectedBluetooth ?? bluetooth.devices.first?.id else {
            note("Scan and choose your Bluetooth MSR X6 first.", kind: .error); return
        }
        disconnect(silent: true)
        operationGeneration += 1
        let generation = operationGeneration
        busy = true
        note("Connecting to the Bluetooth MSR X6…", kind: .working)
        operation = Task { [self] in
            do {
                let channel = try await bluetooth.connect(id)
                try Task.checkCancellation()
                transport = channel
                channel.onDisconnect = { [weak self] in
                    self?.disconnect(silent: true)
                    self?.note("Bluetooth writer disconnected. Scan and reconnect.", kind: .error)
                }
                let session = MSRClient(transport: channel)
                client = session
                try await session.handshake()
                try Task.checkCancellation()
                connected = true
                note("MSR X6 connected over Bluetooth and responding.", kind: .success)
            } catch { if generation == operationGeneration { failed(error) } }
            if generation == operationGeneration { busy = false; operation = nil }
        }
    }
    func disconnect(silent: Bool = false) {
        operationGeneration += 1
        operation?.cancel(); operation = nil
        transport?.cancel()
        transport?.close()
        transport = nil; client = nil
        bluetooth.close()
        connected = false; busy = false
        if !silent { note("Writer disconnected.") }
    }
    func cancel() {
        keyboardArmed = false
        let wrote = writeAcknowledged
        disconnect(silent: true)
        note(wrote ? "Write reported success; read-back verification was cancelled. Reconnect to verify." : "Operation cancelled. Reconnect the writer before the next operation.")
    }
    func loadPreset(original: Bool) {
        guard !busy else { return }
        drafts = (original ? CardData.originalTest : CardData.shortTest).tracks
        if original { densities = [210, 75, 210] }
    }
    func write() {
        guard !busy, let client, connected else { return }
        let card = CardData(drafts).normalized
        let configuration = settings
        do { try card.validate(densities: configuration.densities) }
        catch { note(error.localizedDescription, kind: .error); return }
        keyboardArmed = false; busy = true; writeAcknowledged = false
        operationGeneration += 1
        let generation = operationGeneration
        expectedCard = card; readCard = nil; verification = []
        operation = Task { [self] in
            do {
                note("Applying density and \(configuration.highCoercivity ? "HiCo" : "LoCo") settings…", kind: .working)
                try await client.configure(configuration)
                try Task.checkCancellation()
                note("Ready to write — swipe your card through the connected writer.", kind: .working)
                try await client.write(card, settings: configuration)
                writeAcknowledged = true
                try Task.checkCancellation()
                note("Write reported success. Swipe the SAME card again to verify.", kind: .working)
                let result = try await client.read()
                try Task.checkCancellation()
                acceptRead(result, expected: card)
                writeAcknowledged = false
            } catch { if generation == operationGeneration { failed(error) } }
            if generation == operationGeneration { busy = false; operation = nil }
        }
    }
    func readUSB(compare: Bool = false) {
        guard !busy, let client, connected else { return }
        let expected = compare ? CardData(drafts).normalized : nil
        keyboardArmed = false; busy = true; verification = []; readCard = nil
        expectedCard = expected; writeAcknowledged = false
        operationGeneration += 1
        let generation = operationGeneration
        let configuration = settings
        operation = Task { [self] in
            do {
                // Explicitly set BPC and density so settings left by another utility do not leak in.
                try await client.configure(configuration)
                note("Ready to read — swipe a card through the connected writer.", kind: .working)
                let result = try await client.read()
                try Task.checkCancellation()
                acceptRead(result, expected: expected)
            } catch { if generation == operationGeneration { failed(error) } }
            if generation == operationGeneration { busy = false; operation = nil }
        }
    }
    func armKeyboard() {
        guard !busy else { return }
        expectedCard = nil; readCard = nil; verification = []
        rawSwipe = ""
        captureGeneration += 1
        keyboardArmed = true
        note("Reader ready. Keep this window active and swipe through the MSR600.", kind: .working)
    }
    func acceptKeyboard(_ text: String) {
        guard keyboardArmed, !busy else { return }
        keyboardArmed = false
        rawSwipe = text
        reparseSwipe()
    }
    func reparseSwipe() {
        guard !rawSwipe.isEmpty else { return }
        readCard = nil; verification = []
        do { acceptRead(try KeyboardSwipeParser.parse(rawSwipe, format: inputFormat), expected: nil) }
        catch { note(error.localizedDescription, kind: .error) }
    }
    func keyboardError(_ text: String) { keyboardArmed = false; note(text, kind: .error) }
    func stopKeyboard() { keyboardArmed = false; note("Reader capture stopped.") }
    private func acceptRead(_ card: CardData, expected: CardData?) {
        readCard = card; receivedAt = Date()
        if let expected {
            verification = (0..<3).map { expected.tracks[$0].isEmpty ? nil : card.tracks[$0] == expected.tracks[$0] }
            let okay = !verification.contains(false)
            note(okay ? "Verified — every requested track matches." : "Verification failed — one or more tracks differ. Check the results below.", kind: okay ? .success : .error)
        } else {
            verification = []
            let count = card.tracks.filter { !$0.isEmpty }.count
            note("Read complete — received \(count) track\(count == 1 ? "" : "s").", kind: .success)
        }
    }
    func useReadData() {
        guard !busy, let card = readCard else { return }
        drafts = card.tracks; selectedPage = .write
        note("Read data loaded into the track editors.")
    }
    private func failed(_ error: Error) {
        if error is CancellationError { return }
        let prefix = writeAcknowledged ? "Write reported success, but verification did not complete. " : ""
        transport?.cancel(); transport?.close(); transport = nil; client = nil; connected = false
        note(prefix + error.localizedDescription, kind: .error)
    }
    func copyResults() {
        guard let card = readCard else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString((0..<3).map { "Track \($0 + 1): \(card.framed($0))" }.joined(separator: "\n"), forType: .string)
    }
    func clearSession() {
        guard !busy, !nfc.busy, !uhf.busy, !uhf.scanning, !lf.busy else { return }
        nfc.clear(); uhf.clear(); lf.clear()
        keyboardArmed = false; drafts = ["", "", ""]; readCard = nil; expectedCard = nil
        verification = []; log = []; receivedAt = nil; status = "Session cleared."; statusKind = .neutral
        rawSwipe = ""
    }
}
