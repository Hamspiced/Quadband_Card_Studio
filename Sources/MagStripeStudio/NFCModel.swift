// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import Combine
import MagStripeCore

@MainActor final class NFCModel: ObservableObject {
    enum HFPage: String, CaseIterable { case read = "Read", write = "Write records", tools = "Tools", advanced = "Advanced" }
    @Published var hfPage = HFPage.read
    @Published var automaticKeys = true
    @Published var ndef: ClassicNDEF?
    @Published var ndefMessage = ""
    @Published var recordQueue: [NDEFRecord] = []
    @Published var readerSettings: ReaderSettingsSnapshot?
    @Published var readerEdits: [String:Int] = [:]
    @Published var readerConfigResults: [ReaderApplyResult] = []
    @Published var readerConfigPreview = false
    let readerConfigService: any ReaderConfigServicing = ReaderConfigService()
    var autoDiscovery = true
    @Published var readers: [String] = []
    @Published var selectedReader = ""
    @Published var busy = false
    @Published var writing = false
    @Published var status = "Connect an ACS reader or Chameleon Ultra, then place your Classic 1K card on it."
    @Published var statusKind = AppModel.StatusKind.neutral
    @Published var key = "FF FF FF FF FF FF"
    @Published var keyType = ClassicKeyType.a
    @Published var allSectors = true
    @Published var selectedSector = 0
    @Published var selectedBlock = 1
    @Published var draft = ""
    @Published var snapshot: ClassicSnapshot?
    @Published var profile: CardCredentials?
    @Published var profileOrigin = ""
    @Published var batchPresented = false
    @Published var batchRunning = false
    @Published var copyResults: [NFCCopyResult] = []
    @Published var copiesSucceeded = 0
    @Published var copiesFailed = 0
    var snapshotReader = ""
    let service: any NFCServicing
    let pollingInterval: UInt64
    init(service: any NFCServicing = NFCService(), pollingInterval: UInt64 = 300_000_000) {
        self.service = service; self.pollingInterval = pollingInterval
    }
    var canSaveCredentials: Bool {
        guard let snapshot else { return false }
        return (try? CardCredentials(snapshot: snapshot)) != nil
    }
    var operation: Task<Void, Never>?

    var draftError: String? {
        do { _ = try HexBytes.parse(draft, count: 16); _ = try HexBytes.parse(key, count: 6); return nil }
        catch { return error.localizedDescription }
    }
    var canWrite: Bool {
        !busy && !selectedReader.isEmpty && snapshotReader == selectedReader && snapshot?.card.isClassic1K == true &&
            snapshot?.blocks[selectedBlock] != nil && ClassicClient.isDataBlock(selectedBlock) && draftError == nil
    }
    func refresh() {
        guard !busy else { return }
        busy = true
        operation = Task {
            defer { busy = false; operation = nil }
            do {
                readers = try await service.readers()
                if !readers.contains(selectedReader) { selectedReader = readers.first(where: ReaderFamily.isContactless) ?? readers.first ?? ""; readerChanged() }
                status = readers.isEmpty ? "No HF reader found. Connect an ACS reader, or connect your Chameleon in LF Workspace, then refresh." : "Reader found. Place a card on it, then choose Read card."
                statusKind = .neutral
            } catch { failed(error) }
        }
    }
    func readerChanged() {
        ndef = nil; ndefMessage = ""; snapshot = nil; snapshotReader = ""; draft = ""
        if readerSettings?.reader != selectedReader { readerSettings = nil; readerEdits = [:]; readerConfigResults = []; readerConfigPreview = false }
    }
    func selectSector(_ sector: Int) {
        selectedSector = sector
        selectBlock(sector == 0 ? 1 : sector * 4)
    }
    func selectBlock(_ block: Int) {
        selectedBlock = block
        draft = snapshot?.blocks[block].map(HexBytes.string) ?? ""
    }
    func read(identifyOnly: Bool = false, forProfile: Bool = false) {
        guard !busy, !selectedReader.isEmpty else { return }
        let keyBytes: [UInt8]
        do { keyBytes = identifyOnly ? [] : try HexBytes.parse(key, count: 6) }
        catch { failed(error); return }
        let reader = selectedReader, type = keyType, sector = (allSectors || forProfile) ? nil : selectedSector
        if forProfile { profile = nil; profileOrigin = "" }
        readerChanged(); busy = true; statusKind = .working
        status = identifyOnly ? "Reading card details…" : "Reading card memory. Keep the card on the reader…"
        operation = Task {
            defer { busy = false; operation = nil }
            do {
                let result: ClassicSnapshot
                if identifyOnly { result = ClassicSnapshot(card: try await service.identify(reader: reader)) }
                else if automaticKeys { result = try await service.readAutomatic(reader: reader, key: keyBytes, type: type, sector: sector) }
                else { result = try await service.read(reader: reader, key: keyBytes, type: type, sector: sector) }
                try Task.checkCancellation()
                snapshot = result; snapshotReader = reader
                selectBlock(selectedBlock)
                inspectRecords(result)
                if forProfile {
                    profile = try CardCredentials(snapshot: result)
                    profileOrigin = "Scanned card"
                }
                if identifyOnly {
                    status = "Card identified: \(result.card.name)."; statusKind = .success
                } else {
                    if let ndef { status = "Read complete — \(ndef.records.count) NFC record\(ndef.records.count == 1 ? "" : "s")."; statusKind = .success; return }
                    status = "Read \(result.blocks.count) blocks" + (result.errors.isEmpty ? "." : "; \(result.errors.count) could not be read. Check the selected key and access permissions.")
                    statusKind = result.errors.isEmpty ? .success : .error
                }
            } catch { failed(error) }
        }
    }
    func write() {
        guard canWrite, let snapshot, let previous = snapshot.blocks[selectedBlock] else { return }
        let bytes: [UInt8], keyBytes: [UInt8]
        do { bytes = try HexBytes.parse(draft, count: 16); keyBytes = try (automaticKeys ? snapshot.sectorKeys[selectedBlock / 4]?.bytes : nil) ?? HexBytes.parse(key, count: 6) }
        catch { failed(error); return }
        let reader = selectedReader, block = selectedBlock, type = (automaticKeys ? snapshot.sectorKeys[selectedBlock / 4]?.type : nil) ?? keyType
        busy = true; writing = true; statusKind = .working
        status = "Writing block \(block) and verifying. Keep the card on the reader…"
        operation = Task {
            defer { busy = false; writing = false; operation = nil }
            do {
                let verified = try await service.write(reader: reader, block: block, data: bytes, key: keyBytes,
                                                      type: type, expected: snapshot.card, previous: previous)
                self.snapshot?.blocks[block] = verified
                if let updated = self.snapshot { inspectRecords(updated) }
                draft = HexBytes.string(verified)
                status = "Verified — block \(block) matches all 16 requested bytes."; statusKind = .success
            } catch {
                readerChanged()
                failed(error)
            }
        }
    }
    func cancel() {
        guard !writing || batchRunning else { return }
        operation?.cancel()
        status = "Stopping NFC read…"
    }
    func failed(_ error: Error) {
        status = error is CancellationError ? "NFC operation cancelled." : error.localizedDescription
        statusKind = error is CancellationError ? .neutral : .error
    }
    func clear() {
        guard !busy else { return }
        recordQueue = []; ndef = nil; ndefMessage = ""
        readerSettings = nil; readerEdits = [:]; readerConfigResults = []; readerConfigPreview = false
        profile = nil; profileOrigin = ""; copyResults = []; copiesSucceeded = 0; copiesFailed = 0
        readerChanged(); key = "FF FF FF FF FF FF"; keyType = .a
        status = "NFC session cleared."; statusKind = .neutral
    }
}
