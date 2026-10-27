// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import MagStripeCore

extension NFCModel {
    var recordBytes: Int { (try? NDEFCodec.encode(recordQueue).count) ?? 0 }
    var recordWriteError: String? {
        guard let ndef else { return "Read an NFC-formatted card before writing records." }
        guard ndef.writable else { return "The card is read-only with the available key." }
        do { _ = try ndef.replacing(with: recordQueue); return nil }
        catch { return error.localizedDescription }
    }
    func inspectRecords(_ snapshot: ClassicSnapshot) {
        do { ndef = try ClassicNDEF(snapshot: snapshot); ndefMessage = "" }
        catch { ndef = nil; ndefMessage = error.localizedDescription }
    }
    func readRecords() { allSectors = true; read() }
    func useExistingRecords() {
        guard !busy, let ndef else { return }
        recordQueue = ndef.records; hfPage = .write
    }
    func writeRecords() {
        guard !busy, !selectedReader.isEmpty, let expected = ndef, recordWriteError == nil else { return }
        let keyBytes: [UInt8]
        do { keyBytes = try HexBytes.parse(key, count: 6) } catch { failed(error); return }
        let records = recordQueue, reader = selectedReader, type = keyType
        busy = true; writing = true; statusKind = .working
        status = "Writing NFC records and verifying. Keep the card on the reader…"
        operation = Task {
            defer { busy = false; writing = false; operation = nil }
            do {
                let result = try await service.writeNDEF(reader: reader, records: records, expected: expected, key: keyBytes, type: type)
                snapshot = result; snapshotReader = reader; inspectRecords(result); selectBlock(selectedBlock)
                status = "Verified — all \(records.count) NFC records match."; statusKind = .success
            } catch { readerChanged(); failed(error) }
        }
    }
}
