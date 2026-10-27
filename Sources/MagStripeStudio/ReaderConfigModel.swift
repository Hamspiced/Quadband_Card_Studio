// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import AppKit
import MagStripeCore

extension NFCModel {
    var readerConfigPlan: Result<[ReaderConfigChange],Error> {
        Result {
            guard let settings = readerSettings, settings.reader == selectedReader else { throw StripeError("Read the selected reader's settings first.") }
            return try ReaderConfigPlan.make(snapshot:settings,edits:readerEdits)
        }
    }
    func readReaderSettings() {
        guard !busy, ReaderFamily.detect(selectedReader) != nil else { return }
        let reader = selectedReader
        busy = true; statusKind = .working; status = "Reading reader settings…"
        readerSettings = nil; readerEdits = [:]; readerConfigResults = []; readerConfigPreview = false
        operation = Task {
            defer { busy = false; operation = nil }
            do {
                let settings = try await readerConfigService.read(reader:reader)
                try Task.checkCancellation()
                readerSettings = settings
                status = "\(settings.family.rawValue) settings loaded. Choose changes, then review and apply."
                statusKind = .success
            } catch { failed(error) }
        }
    }
    func stageReaderPreset(_ name: String) {
        guard !busy, let settings = readerSettings else { return }
        readerEdits.merge(ReaderSettingsCatalog.preset(name,family:settings.family,snapshot:settings)) { _,new in new }
    }
    func applyReaderSettings() {
        guard !busy, let settings = readerSettings, settings.reader == selectedReader,
              case .success(let plan) = readerConfigPlan, !plan.isEmpty else { return }
        let edits = readerEdits
        busy = true; writing = true; readerConfigPreview = false; readerConfigResults = []
        status = "Applying reader settings…"; statusKind = .working
        // Reader configuration can restart the RF interface; previous card reads are now stale.
        readerChanged()
        operation = Task {
            defer { busy = false; writing = false; operation = nil }
            do {
                readerConfigResults = try await readerConfigService.apply(snapshot:settings,edits:edits)
                let completed = readerConfigResults.count == plan.count && readerConfigResults.allSatisfy(\.success)
                status = completed ? "Reader changes applied. " + (plan.contains(where: { $0.register.replug }) ? "Unplug and reconnect to activate the saved settings." : "See the results below for verification details.") : "Some reader changes could not be verified. See the results, then read settings again."
                statusKind = completed ? .success : .error
                readerSettings = nil; readerEdits = [:]
            } catch { readerSettings = nil; readerEdits = [:]; failed(error) }
        }
    }
    func exportReaderScript() {
        let source = Bundle.main.url(forResource:"acr1552u_rf_config",withExtension:"py")
            ?? URL(fileURLWithPath:FileManager.default.currentDirectoryPath).appendingPathComponent("Resources/acr1552u_rf_config.py")
        let panel = NSSavePanel(); panel.nameFieldStringValue = "acr1552u_rf_config.py"; panel.title = "Save the supplied Tototo31 / Ashsh script"
        guard panel.runModal() == .OK, let destination = panel.url else { return }
        do { try Data(contentsOf:source).write(to:destination,options:.atomic) }
        catch { failed(error) }
    }
}
