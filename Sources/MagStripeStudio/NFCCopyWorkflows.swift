// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import AppKit
import UniformTypeIdentifiers
import MagStripeCore

struct NFCCopyResult: Identifiable {
    let id = UUID()
    let number: Int
    let time = Date()
    let success: Bool
    let detail: String
}

extension NFCModel {
    private var credentialsType: UTType { UTType(exportedAs: "com.midwestgadgets.quadband.cardcredentials", conformingTo: .json) }
    func saveCredentials() {
        guard !busy, let snapshot else { return }
        do {
            let credentials = try CardCredentials(snapshot: snapshot)
            let data = try credentials.encoded()
            let panel = NSSavePanel()
            panel.title = "Save card credentials"
            panel.message = "Saves the 47 data blocks only. Card UID and sector keys are not included."
            panel.allowedContentTypes = [credentialsType]
            panel.nameFieldStringValue = "Classic-1K-credentials.qcard"
            guard panel.runModal() == .OK, let url = panel.url else { return }
            try data.write(to: url, options: [.atomic])
            profile = credentials; profileOrigin = url.lastPathComponent
            status = "Saved 47 data blocks to \(url.lastPathComponent)."; statusKind = .success
        } catch { failed(error) }
    }
    func chooseCredentials() {
        guard !busy else { return }
        let panel = NSOpenPanel()
        panel.title = "Upload card credentials"
        panel.message = "Choose a Quadband .qcard file. It stays on this Mac."
        panel.allowedContentTypes = [credentialsType, .json]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        profile = nil; profileOrigin = ""
        do {
            // Limit the file before allocating its contents.
            let values = try url.resourceValues(forKeys: [.fileSizeKey])
            guard let size = values.fileSize, size <= 65536 else { throw StripeError("Choose a Quadband credentials file smaller than 64 KB.") }
            let validated = try CardCredentials.decode(Data(contentsOf: url))
            profile = validated; profileOrigin = url.lastPathComponent
            status = "Loaded 47 data blocks. Place a target card on the reader, then Write profile & verify."; statusKind = .neutral
        } catch { failed(error) }
    }
    func writeProfileOnce() {
        guard !busy, !selectedReader.isEmpty, let profile else { return }
        let keyBytes: [UInt8]
        do { keyBytes = try HexBytes.parse(key, count: 6); _ = try profile.validatedBlocks() }
        catch { failed(error); return }
        let reader = selectedReader, type = keyType
        busy = true; writing = true; readerChanged()
        status = "Writing 47 data blocks and verifying. Keep the target card on the reader…"; statusKind = .working
        operation = Task {
            defer { busy = false; writing = false; operation = nil }
            do {
                let card = try await service.writeProfile(reader: reader, profile: profile, key: keyBytes, type: type)
                status = "Verified — all 47 data blocks match. Target card ID: \(HexBytes.string(card.uid))."; statusKind = .success
            } catch { failed(error) }
        }
    }
    func startBatch() {
        guard !busy, !selectedReader.isEmpty, let profile else { return }
        let keyBytes: [UInt8]
        do { keyBytes = try HexBytes.parse(key, count: 6); _ = try profile.validatedBlocks() }
        catch { failed(error); return }
        let reader = selectedReader, type = keyType
        copiesSucceeded = 0; copiesFailed = 0; copyResults = []
        busy = true; batchRunning = true; readerChanged()
        status = "Starting Copy to infinity…"; statusKind = .working
        operation = Task {
            defer { busy = false; writing = false; batchRunning = false; operation = nil }
            // Require an observed empty reader before the first target, even for identical UIDs.
            var needsRemoval = true
            while !Task.isCancelled {
                do {
                    let present = try await service.isPresent(reader: reader)
                    try Task.checkCancellation()
                    statusKind = .working
                    if needsRemoval {
                        if present { status = "Remove the current card. The batch is waiting for the next target." }
                        else { needsRemoval = false; status = "Ready — place the next target card on the reader." }
                    } else if present {
                        writing = true
                        let number = copiesSucceeded + copiesFailed + 1
                        status = "Copying card \(number). Keep it on the reader until verification finishes…"
                        do {
                            let card = try await service.writeProfile(reader: reader, profile: profile, key: keyBytes, type: type)
                            copiesSucceeded += 1
                            appendCopyResult(number: number, success: true, detail: "47 blocks verified · ID \(HexBytes.string(card.uid))")
                        } catch {
                            copiesFailed += 1
                            appendCopyResult(number: number, success: false, detail: error.localizedDescription)
                        }
                        writing = false; needsRemoval = true
                        if !Task.isCancelled {
                            status = "Card \(number) finished. Remove it, then place the next card."
                            statusKind = .working
                        }
                    }
                } catch {
                    if Task.isCancelled { break }
                    // A disconnected reader can be reconnected without discarding the batch.
                    status = "Batch is waiting for the reader. \(error.localizedDescription)"
                    statusKind = .error
                    needsRemoval = true
                }
                do { try await Task.sleep(nanoseconds: pollingInterval) } catch { break }
            }
            status = "Copy to infinity stopped. \(copiesSucceeded) succeeded; \(copiesFailed) failed."
            statusKind = copiesFailed > 0 ? .error : .neutral
        }
    }
    func stopBatch() {
        guard batchRunning else { return }
        operation?.cancel()
        status = "Stopping after the current reader command…"
    }
    private func appendCopyResult(number: Int, success: Bool, detail: String) {
        copyResults.insert(NFCCopyResult(number: number, success: success, detail: detail), at: 0)
        // Keep a bounded on-screen history; lifetime totals remain accurate.
        if copyResults.count > 500 { copyResults.removeLast() }
    }
}
