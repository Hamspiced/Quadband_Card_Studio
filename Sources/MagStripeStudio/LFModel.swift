// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit
import MagStripeCore

@MainActor final class LFModel: ObservableObject {
    enum Link: String, CaseIterable { case usb = "USB", bluetooth = "Bluetooth" }
    @Published var link = Link.usb
    @Published var ports: [SerialDevice] = []
    @Published var selectedPort = ""
    @Published var selectedBluetooth: UUID?
    let bluetooth = ChameleonBluetooth()
    @Published var connected = false
    @Published var busy = false
    @Published var hfWorkflowBusy = false
    @Published var info: ChameleonInfo?
    @Published var status = "Connect a Chameleon Ultra to read LF cards and write T5577 tags."
    @Published var statusKind = AppModel.StatusKind.neutral
    @Published var format: LFFormat?
    @Published var hidHint = 0
    @Published var card: LFCard?
    @Published var defaultID = "0102030405"
    @Published var preview: LFWriteIntent?
    @Published var log: [String] = []
    var autoDiscovery = true
    private var connection: ChameleonConnection?
    private var client: ChameleonClient?
    private var operation: Task<Void,Never>?
    private var generation = UUID()
    private var connectionIdentity = UUID()
    var canOperate: Bool { connected && !busy && !hfWorkflowBusy }
    var canClone: Bool { canOperate && card.map { info?.canWrite($0) == true } == true }
    var canWriteDefault: Bool { canOperate && info?.capabilities.contains(3016) == true && info?.capabilities.contains(3000) == true }
    func refresh() {
        guard !connected && !busy else { return }
        ports = SerialDevice.scan()
        if !ports.contains(where: { $0.path == selectedPort }) { selectedPort = ports.first(where: { $0.name.localizedCaseInsensitiveContains("chameleon") || $0.vendor == 0x6868 })?.path ?? ports.first?.path ?? "" }
    }
    func note(_ text: String, _ kind: AppModel.StatusKind = .neutral) {
        status = text; statusKind = kind; log.insert("\(Date().formatted(date:.omitted,time:.standard))  \(text)",at:0); log = Array(log.prefix(60))
    }
    func connect() {
        guard !busy && !hfWorkflowBusy && !connected else { return }
        let link = link, port = selectedPort, id = selectedBluetooth
        run("Connecting to Chameleon over \(link.rawValue)…") {
            let value: ChameleonConnection
            if link == .usb { value = try ChameleonConnection(path:port) }
            else {
                guard let id else { throw StripeError("Scan for Bluetooth devices and select your Chameleon.") }
                value = ChameleonConnection(channel:try await self.bluetooth.connect(id))
            }
            self.connection = value
            self.observeDisconnect(value)
            let client = ChameleonClient(transport:value)
            do {
                let info = try await client.identify(); try Task.checkCancellation()
                self.client = client; self.info = info; self.connected = true; self.format = nil
                self.note("Chameleon Ultra connected by \(link.rawValue). Ready to scan.",.success)
            } catch { value.close(); self.connection = nil; self.client = nil; self.info = nil; self.connected = false; throw error }
        }
    }
    private func observeDisconnect(_ value: ChameleonConnection) {
        value.onDisconnect = { [weak self] error in
            self?.connected = false; self?.preview = nil; self?.note(error.localizedDescription,.error)
        }
    }
    func disconnect() {
        guard !busy && !hfWorkflowBusy else { return }
        run("Disconnecting Chameleon…") {
            var message = "Chameleon disconnected."
            if let client = self.client, self.connected {
                do { try await client.restoreMode() } catch { message += " Previous device mode could not be restored." }
            }
            self.close(); self.note(message)
        }
    }
    func close() {
        connectionIdentity = UUID(); connection?.close(); connection = nil; client = nil; bluetooth.close(); connected = false; info = nil; preview = nil
    }
    func cancel() {
        generation = UUID(); operation?.cancel(); operation = nil; close(); busy = false
        note("Operation cancelled and connection closed. If a write had started, scan the target before trying again.")
    }
    func scan() {
        guard canOperate, let client else { return }
        let selected = format, hint = UInt8(hidHint)
        card = nil; preview = nil
        run("Scanning LF card…") {
            guard let result = try await client.scan(format:selected,hidHint:hint) else { self.note("No supported LF card found. Center one card on the LF antenna and try again."); return }
            self.card = result; self.note("Read \(result.format.rawValue) card. Ready to save or clone.",.success)
        }
    }
    func prepare(_ action: LFWriteIntent.Action) {
        guard canOperate else { return }
        do {
            let target: LFCard
            if action == .clone { guard let card else { throw StripeError("Scan a source card or open an LF card file first.") }; target = card }
            else { target = try LFCard(format:.em410x,bytes:HexBytes.parse(defaultID,count:5)) }
            guard info?.canWrite(target) == true else { throw StripeError("This firmware cannot write \(target.format.rawValue).") }
            _ = try T5577.blocks(target)
            preview = LFWriteIntent(action:action,card:target,connectionID:connectionIdentity)
        } catch { note(error.localizedDescription,.error) }
    }
    func write(_ intent: LFWriteIntent) {
        guard canOperate, let client, connectionIdentity == intent.connectionID else { preview = nil; return }
        preview = nil
        run(intent.action == .recover ? "Restoring T5577 configuration and verifying its ID…" : "Writing T5577 and reading it back…") {
            _ = try await client.writeAndVerify(intent.card)
            self.note(intent.action == .recover ? "Recovery verified: the tag now reads as \(intent.card.identifier)." : "T5577 write verified: \(intent.card.identifier).",.success)
        }
    }
    func clear() {
        guard !busy else { return }; card = nil; preview = nil; defaultID = "0102030405"; log = []; note("LF session cleared.")
    }
    func save() {
        guard let card, !busy else { return }
        let panel = NSSavePanel(); panel.title = "Save LF card"; panel.nameFieldStringValue = "LF-card.qlf"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try card.fileData().write(to:url,options:.atomic); note("LF card saved. No password data is included.",.success) } catch { note(error.localizedDescription,.error) }
    }
    func load() {
        guard !busy else { return }
        let panel = NSOpenPanel(); panel.title = "Open LF card"; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            let size = try url.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? 0
            guard size <= 16_384 else { throw StripeError("LF card file is too large.") }
            card = try LFCard.fromFile(Data(contentsOf:url)); preview = nil; note("LF card file loaded. Review the source, then place a T5577 target to clone.",.success)
        } catch { note(error.localizedDescription,.error) }
    }
    var hfReaderName: String? { connected && info?.capabilities.contains(2000) == true ? "Chameleon Ultra · " + link.rawValue : nil }
    func withHF<T>(_ work: @escaping @MainActor (ChameleonHFClient) async throws -> T) async throws -> T {
        guard connected && !busy, let client else { throw StripeError("Chameleon is busy or disconnected. Finish the current operation before using HF Workspace.") }
        busy = true; preview = nil; let token = generation
        note("Chameleon is in use by HF Workspace…",.working)
        defer { if generation == token { busy = false } }
        do {
            let value = try await work(ChameleonHFClient(client:client))
            if generation == token { note("HF operation finished. Chameleon is ready.") }; return value
        } catch { if generation == token { note("HF operation ended. See HF Workspace for details.") }; throw error }
    }
    private func run(_ text: String, work: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }; busy = true; note(text,.working)
        let token = generation
        operation = Task { [self] in
            do { try await work() } catch { if token == generation { note(error.localizedDescription,.error) } }
            if token == generation { busy = false; operation = nil }
        }
    }
}
struct LFWriteIntent: Identifiable {
    enum Action { case clone, defaults, recover }
    let id = UUID()
    let action: Action
    let card: LFCard
    let connectionID: UUID
    var title: String { switch action { case .clone: return "Clone to T5577"; case .defaults: return "Write default test value"; case .recover: return "Recover T5577" } }
}
