// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit
import MagStripeCore

@MainActor final class UHFModel: ObservableObject {
    @Published var ports: [SerialDevice] = []
    @Published var selectedPort = ""
    @Published var baud = 0
    @Published var address = "0"
    @Published var connected = false
    @Published var busy = false
    @Published var scanning = false
    @Published var writing = false
    @Published var status = "Connect your Piswords UHF reader to get started."
    @Published var statusKind = AppModel.StatusKind.neutral
    @Published var version = ""
    @Published var details = ""
    @Published var tags: [UHFTag] = []
    @Published var selectedTag: String?
    @Published var bank = UHFMemoryBank.user
    @Published var word = "0"
    @Published var count = "6"
    @Published var password = "00000000"
    @Published var memory: UHFMemoryRead?
    @Published var memoryLabel = ""
    @Published var writeData = ""
    @Published var newEPC = ""
    @Published var writePreview: UHFWriteIntent?
    @Published var power = 20
    @Published var log: [String] = []
    var autoDiscovery = true
    private var channel: PiswordsTransport?
    private var client: UHFClient?
    private var operation: Task<Void,Never>?
    private var generation = 0
    var tag: UHFTag? { tags.first { $0.id == selectedTag } }
    var canOperate: Bool { connected && !busy && !scanning }
    func refresh() {
        guard !busy && !connected else { return }
        ports = SerialDevice.scan()
        if !ports.contains(where: { $0.path == selectedPort }) {
            selectedPort = ports.first(where: { $0.vendor == 0x1A86 && $0.product == 0xFE0C })?.path ?? ports.first?.path ?? ""
        }
    }
    func note(_ text: String, _ kind: AppModel.StatusKind = .neutral) {
        status = text; statusKind = kind; log.insert("\(Date().formatted(date:.omitted,time:.standard))  \(text)",at:0); log = Array(log.prefix(80))
    }
    func connect() {
        guard !busy, !selectedPort.isEmpty else { return }
        guard let addr = UInt8(address), addr < 255 else { note("Enter a reader address from 0 to 254.",.error); return }
        disconnect(silent:true)
        let port = selectedPort, rates = baud == 0 ? [9600,115200,57600,38400,19200,230400] : [baud]
        generation += 1; let token = generation
        busy = true; note("Checking the Piswords connection…",.working)
        operation = Task { [self] in
            var lastError: Error = StripeError("No compatible reader responded.")
            for rate in rates {
                if Task.isCancelled || token != generation { break }
                var candidate: PiswordsTransport?
                do {
                    let transport = try PiswordsTransport(path:port,baud:rate,address:addr); candidate = transport
                    let response = try await transport.exchange(0x72,data:[],timeout:0.8)
                    let firmware = try UHFProtocol.version(response)
                    try Task.checkCancellation()
                    guard token == generation else { transport.close(); return }
                    channel = transport; client = UHFClient(transport:transport)
                    transport.onTag = { [weak self] in self?.accept($0) }
                    transport.onDisconnect = { [weak self] error in self?.connectionLost(error) }
                    connected = true; baud = rate; version = firmware
                    try await loadDetails()
                    note("Piswords connected at \(rate) baud. Ready to scan.",.success)
                    busy = false; operation = nil; return
                } catch { candidate?.close(); lastError = error }
            }
            if token == generation { connected = false; channel?.close(); channel = nil; client = nil; busy = false; operation = nil; note(lastError.localizedDescription,.error) }
        }
    }
    private func connectionLost(_ error: Error) {
        connected = false; scanning = false
        note(error.localizedDescription,.error)
    }
    private func loadDetails() async throws {
        guard let client else { return }
        var pieces: [String] = []
        let ant = try await client.command(0x75)
        if ant.count == 1 && (1...8).contains(ant[0]) { pieces.append("Antenna \(ant[0])") }
        let rf = try await client.command(0x77)
        if [1,4,8].contains(rf.count), rf.allSatisfy({ $0 <= 33 }) { power = Int(rf[0]); pieces.append("\(rf.map(String.init).joined(separator:" / ")) dBm") }
        let region = try await client.command(0x79)
        if region.count >= 3 { pieces.append("Region \(region[0]) · channels \(region[1])–\(region[2])") }
        details = pieces.joined(separator:"  ·  ")
    }
    func disconnect(silent: Bool = false) {
        generation += 1; operation?.cancel(); operation = nil; channel?.close(); channel = nil; client = nil
        connected = false; scanning = false; busy = false; writing = false; version = ""; details = ""; tags = []; selectedTag = nil; memory = nil; writePreview = nil
        if !silent { note("UHF reader disconnected.") }
    }
    func scan() {
        guard canOperate, let client else { return }
        run("Starting UHF scan…") {
            // Clear only the reader's access-EPC filter, never tag memory.
            try UHFProtocol.acknowledge(await client.command(0x85,[1]))
            try UHFProtocol.acknowledge(await client.command(0x89,[1]))
            self.scanning = true; self.note("Scanning UHF tags. Stop scanning to read memory or edit an EPC.",.working)
        }
    }
    func stop() {
        guard scanning && !busy, let client else { return }
        run("Stopping UHF scan…") {
            try UHFProtocol.acknowledge(await client.command(0x8C))
            self.scanning = false
            self.note("Scan stopped. \(self.tags.count) unique EPC\(self.tags.count == 1 ? "" : "s") found.",.success)
        }
    }
    func accept(_ value: UHFTag) {
        if let index = tags.firstIndex(where: { $0.id == value.id }) {
            var value = value; value.sightings = tags[index].sightings+1; tags[index] = value
        } else if tags.count < 2000 { tags.append(value) }
        if selectedTag == nil { selectedTag = value.id; newEPC = value.id }
    }
    func selectTag(_ id: String) { guard !busy else { return }; selectedTag = id; newEPC = id; memory = nil; writePreview = nil }
    func clear() { guard !busy && !scanning else { return }; tags = []; selectedTag = nil; memory = nil; writeData = ""; newEPC = ""; password = "00000000"; log = []; writePreview = nil; note("UHF session cleared.") }
    func readMemory() {
        guard canOperate, let client, let tag else { return }
        do {
            let args = try memoryArguments(), bank = bank, epc = tag.epc
            memory = nil
            run("Reading \(bank.name)…") {
                self.memory = try await client.read(epc:epc,bank:bank,word:args.word,count:args.count,password:args.password)
                self.memoryLabel = "\(bank.name) · word \(args.word) · \(args.count) words"
                self.note("Read \(args.count*2) bytes from the selected tag.",.success)
            }
        } catch { note(error.localizedDescription,.error) }
    }
    private func memoryArguments() throws -> (word: UInt32,count: Int,password:[UInt8]) {
        guard let word = UInt32(word), let count = Int(count), (1...64).contains(count) else { throw StripeError("Enter a starting word and a read length from 1 to 64 words. A word is two bytes.") }
        return (word,count,try HexBytes.parse(password,count:4))
    }
    func prepareWrite(epc: Bool) {
        guard canOperate, let tag else { return }
        do {
            let pass = try HexBytes.parse(password,count:4)
            if epc {
                let bytes = try UHFProtocol.hex(newEPC,maxBytes:62)
                _ = try UHFProtocol.epcPayload(bytes,currentPC:tag.pc)
                guard bytes != tag.epc else { throw StripeError("Enter a different EPC to write.") }
                writePreview = UHFWriteIntent(oldEPC:tag.epc,bytes:bytes,word:1,password:pass,isEPC:true)
            } else {
                guard let offset = UInt32(word) else { throw StripeError("Enter a valid starting word.") }
                let bytes = try UHFProtocol.hex(writeData)
                _ = try UHFProtocol.writeCommand(bank:.user,word:offset,bytes:bytes,password:pass)
                writePreview = UHFWriteIntent(oldEPC:tag.epc,bytes:bytes,word:offset,password:pass,isEPC:false)
            }
        } catch { note(error.localizedDescription,.error) }
    }
    func performWrite(_ intent: UHFWriteIntent) {
        guard canOperate, let client, tag?.epc == intent.oldEPC else { writePreview = nil; return }
        writePreview = nil; writing = true; memory = nil
        run(intent.isEPC ? "Writing and verifying the EPC…" : "Writing and verifying User memory…") {
            let result: UHFMemoryRead
            if intent.isEPC { result = try await client.changeEPC(from:intent.oldEPC,to:intent.bytes,password:intent.password) }
            else { result = try await client.write(epc:intent.oldEPC,bank:.user,word:intent.word,bytes:intent.bytes,password:intent.password) }
            self.memory = result
            self.memoryLabel = intent.isEPC ? "Verified EPC and protocol-control word" : "Verified User memory · word \(intent.word)"
            if intent.isEPC {
                self.tags.removeAll { $0.epc == intent.oldEPC }
                let tag = UHFTag(epc:intent.bytes,pc:result.pc)
                self.accept(tag); self.selectedTag = tag.id; self.newEPC = tag.id
            }
            self.note(intent.isEPC ? "EPC changed and verified by reading it back." : "User memory written and verified.",.success)
        }
    }
    func setBuzzer(_ enabled: Bool) {
        guard canOperate, let client else { return }
        run("Setting the UHF buzzer…") { try UHFProtocol.acknowledge(await client.command(0x7A,[enabled ? 2 : 0])); self.note("Reader acknowledged buzzer \(enabled ? "on" : "off").",.success) }
    }
    func applyPower() {
        guard canOperate, let client, (0...30).contains(power) else { return }
        let value = UInt8(power)
        run("Applying UHF power…") {
            try UHFProtocol.acknowledge(await client.command(0x76,[value]))
            let actual = try await client.command(0x77)
            guard actual == [value] else { throw StripeError("Power could not be verified. Read settings again.") }
            try await self.loadDetails(); self.note("UHF power verified at \(value) dBm.",.success)
        }
    }
    private func run(_ message: String, work: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }
        busy = true; note(message,.working); generation += 1; let token = generation
        operation = Task { [self] in
            do { try await work() } catch { if token == generation { note(error.localizedDescription,.error) } }
            if token == generation { busy = false; writing = false; operation = nil }
        }
    }
    func exportScan() {
        let panel = NSSavePanel(); panel.nameFieldStringValue = "UHF-tag-scan.csv"; panel.title = "Save UHF scan"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let rows = tags.map { "\($0.id),\(HexBytes.string($0.pc)),\($0.antenna),\($0.frequencyKHz),\($0.signal.map(String.init) ?? ""),\($0.sightings)" }
        do { try ("EPC,PC,Antenna,Frequency kHz,Signal dBm,Reads\n"+rows.joined(separator:"\n")).write(to:url,atomically:true,encoding:.utf8) }
        catch { note(error.localizedDescription,.error) }
    }
}
struct UHFWriteIntent: Identifiable {
    let id = UUID()
    let oldEPC: [UInt8]
    let bytes: [UInt8]
    let word: UInt32
    let password: [UInt8]
    let isEPC: Bool
}
