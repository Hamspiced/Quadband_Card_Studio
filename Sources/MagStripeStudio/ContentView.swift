// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit
import MagStripeCore

private let ink = Color(red: 0.09, green: 0.15, blue: 0.22)
private let teal = AppTheme.accent
private let canvas = AppTheme.canvas

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @AppStorage("appearance") private var appearance = AppearanceMode.system.rawValue
    var body: some View {
        HStack(spacing: 0) {
            sidebar.frame(width: 260).environment(\.colorScheme, .dark)
            VStack(spacing: 0) {
                header
                ScrollView {
                    VStack(alignment: .leading, spacing: 20) {
                        if model.selectedPage != .nfc && model.selectedPage != .uhf && model.selectedPage != .lf { statusBanner }
                        switch model.selectedPage {
                        case .write: writer
                        case .read: reader
                        case .nfc: NFCView(model: model.nfc)
                        case .uhf: UHFView(model:model.uhf)
                        case .lf: LFView(model:model.lf)
                        case .help: help
                        }
                        if model.selectedPage != .help && model.selectedPage != .nfc && model.selectedPage != .uhf && model.selectedPage != .lf { activity }
                    }.padding(28)
                }
            }.background(canvas)
        }
        .tint(teal)
        .preferredColorScheme((AppearanceMode(rawValue: appearance) ?? .system).colorScheme)
        .onChange(of: model.selectedPage) { _ in if model.keyboardArmed { model.stopKeyboard() } }
        .onChange(of: model.readerMode) { _ in if model.keyboardArmed { model.stopKeyboard() } }
    }
    private var sidebar: some View {
        ScrollView {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 8) {
                BrandLogo().frame(width: 144, height: 144)
                Text("Quadband").font(.system(size: 24, weight: .bold))
                Text("CARD STUDIO").font(.system(size: 10, weight: .semibold)).tracking(2.4).foregroundColor(.white.opacity(0.7))
                Text("by Midwest Gadgets").font(.system(size: 11)).foregroundColor(.white.opacity(0.55))
                Picker("Appearance", selection: $appearance) {
                    ForEach(AppearanceMode.allCases, id: \.rawValue) { Text($0.rawValue).tag($0.rawValue) }
                }.pickerStyle(.menu).font(.system(size: 11))
                    .accessibilityIdentifier("appearancePicker")
            }.padding(.top, 4)
            VStack(spacing: 4) {
                Text("MAGSTRIPE").font(.system(size: 9, weight: .semibold)).tracking(1.5).foregroundColor(.white.opacity(0.5)).frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 12)
                navigation(.write, icon: "square.and.pencil")
                navigation(.read, icon: "waveform.path")
                Text("CONTACTLESS").font(.system(size: 9, weight: .semibold)).tracking(1.5).foregroundColor(.white.opacity(0.5)).frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 12).padding(.top, 8)
                navigation(.uhf, icon: "antenna.radiowaves.left.and.right")
                navigation(.nfc, icon: "wave.3.right.circle")
                navigation(.lf, icon: "waveform.path.ecg")
                navigation(.help, icon: "questionmark.circle")
            }
            Rectangle().fill(.white.opacity(0.12)).frame(height: 1)
            WriterConnectionView(model: model, bluetooth: model.bluetooth)
            Rectangle().fill(.white.opacity(0.12)).frame(height: 1)
            ChameleonConnectionView(model: model.lf, bluetooth: model.lf.bluetooth)
            Spacer(minLength: 14)
            VStack(alignment: .leading, spacing: 8) {
                Label("Local workspace", systemImage: "desktopcomputer").font(.system(size: 12, weight: .medium))
                Text("Your cards stay local.\nFiles are saved only when you choose.")
                    .font(.system(size: 11)).lineSpacing(4).foregroundColor(.white.opacity(0.5))
                Button("Clear session") { model.clearSession() }.buttonStyle(.plain).foregroundColor(.mint).padding(.top, 6).disabled(model.busy || model.nfc.busy || model.uhf.busy || model.uhf.scanning || model.lf.busy)
            }
        }.padding(18).foregroundColor(.white)
        }.frame(maxHeight: .infinity)
            .background(LinearGradient(colors: [ink, Color(red: 0.07, green: 0.11, blue: 0.17)], startPoint: .topLeading, endPoint: .bottomTrailing))
    }
    private func navigation(_ page: AppModel.AppPage, icon: String) -> some View {
        Button { model.selectedPage = page } label: {
            HStack(spacing: 12) {
                Image(systemName: icon).frame(width: 18)
                Text(page.rawValue).font(.system(size:13, weight: .medium)).fixedSize(horizontal:false,vertical:true)
                Spacer()
                if model.selectedPage == page { Circle().fill(.mint).frame(width: 5, height: 5) }
            }.padding(8).background(model.selectedPage == page ? .white.opacity(0.10) : .clear).cornerRadius(9)
        }.buttonStyle(.plain)
    }
    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 5) {
                Text(model.selectedPage == .write ? "Magstripe Write" : model.selectedPage == .read ? "Magstripe Read" : model.selectedPage == .nfc ? "HF Workspace" : model.selectedPage == .uhf ? "UHF Workspace" : model.selectedPage == .lf ? "LF Workspace" : "Your card workbench")
                    .font(.system(size: 26, weight: .semibold)).foregroundColor(.primary)
                Text(model.selectedPage == .write ? "Prepare three tracks, write, then verify with a second swipe." : model.selectedPage == .read ? "Capture all available tracks from your reader." : model.selectedPage == .nfc ? "Read NFC records, create new ones, and manage your HF cards." : model.selectedPage == .uhf ? "Scan UHF tags, read memory, and write verified EPC changes." : model.selectedPage == .lf ? "Read LF cards, clone to T5577, and restore test tags." : "Set up your reader and make your first test card.")
                    .font(.system(size: 12)).foregroundColor(.secondary)
            }
            Spacer()
            Text(model.selectedPage == .nfc ? "ACS · CHAMELEON" : model.selectedPage == .uhf ? "PISWORDS · UHF" : model.selectedPage == .lf ? "CHAMELEON · LF" : "X6 · MSR600 · MSR605").font(.system(size: 10, weight: .semibold, design: .monospaced)).foregroundColor(teal)
                .padding(.horizontal, 10).padding(.vertical, 7).background(teal.opacity(0.07)).clipShape(Capsule())
        }.padding(.horizontal, 28).padding(.vertical, 24).background(AppTheme.surface)
    }
    private var statusBanner: some View {
        let color: Color = model.statusKind == .error ? .orange : model.statusKind == .success ? teal : .secondary
        return HStack(spacing: 12) {
            if model.busy || model.keyboardArmed { ProgressView().controlSize(.small) }
            else { Image(systemName: model.statusKind == .error ? "exclamationmark.circle" : model.statusKind == .success ? "checkmark.circle.fill" : "info.circle").foregroundColor(color) }
            Text(model.status).font(.system(size: 12, weight: .medium)).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            if model.busy { Button("Cancel") { model.cancel() }.controlSize(.small) }
        }.padding(14).background(color.opacity(0.07)).cornerRadius(10)
            .accessibilityIdentifier("operationStatus")
    }
    private var writer: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("ENCODING").font(.system(size: 10, weight: .semibold)).tracking(1.4).foregroundColor(.secondary)
                Picker("Card", selection: $model.highCoercivity) {
                    Text("HiCo").tag(true); Text("LoCo").tag(false)
                }.pickerStyle(.segmented).frame(width: 150)
                Spacer()
                Menu("Test data") {
                    Button("Short diagnostic data") { model.loadPreset(original: false) }
                    Button("Midwest sample · ISO densities") { model.loadPreset(original: true) }
                }.frame(width: 130)
                Menu("Density preset") {
                    Button("ISO · 210 / 75 / 210") { model.densities = [210, 75, 210] }
                    Button("Custom · 75 / 75 / 75") { model.densities = [75, 75, 75] }
                }.frame(width: 130)
            }.disabled(model.busy)
            ForEach(0..<3, id: \.self) { index in trackEditor(index) }
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: model.validationError == nil ? "checkmark.circle" : "exclamationmark.circle")
                Text(model.validationError ?? "Sentinels are handled automatically. ISO encoding adds parity and LRC.")
                    .fixedSize(horizontal: false, vertical: true)
            }.font(.system(size: 11)).foregroundColor(model.validationError == nil ? .secondary : .orange)
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Use a blank test card.").font(.system(size: 12, weight: .medium))
                    Text("Writing replaces card data. Blank fields may clear those tracks.").font(.system(size: 11)).foregroundColor(.secondary)
                }
                Spacer()
                Button("Verify existing card") { model.readUSB(compare: true) }
                    .disabled(!model.connected || model.busy || model.validationError != nil)
                Button { model.write() } label: { Label("Write & verify", systemImage: "square.and.pencil") }
                    .buttonStyle(.borderedProminent).controlSize(.large)
                    .disabled(!model.connected || model.busy || model.validationError != nil)
            }.padding(.top, 4)
            if model.readCard != nil { results }
        }
    }
    private func trackEditor(_ index: Int) -> some View {
        let count = CardData.payload(model.drafts[index], track: index).utf8.count
        let limit = CardData.payloadLimit(track: index, density: model.densities[index])
        return VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("0\(index + 1)").font(.system(size: 12, weight: .bold, design: .monospaced)).foregroundColor(teal)
                    .frame(width: 30, height: 30).background(teal.opacity(0.07)).cornerRadius(7)
                Text("Track \(index + 1)").font(.system(size: 14, weight: .semibold))
                Text(index == 0 ? "ALPHANUMERIC" : "NUMERIC").font(.system(size: 9, weight: .semibold)).tracking(0.8).foregroundColor(.secondary)
                Spacer()
                Text("\(index == 0 ? 7 : 5) BPC · odd parity").font(.system(size: 10)).foregroundColor(.secondary)
                Picker("Track \(index + 1) density", selection: $model.densities[index]) {
                    Text("75 BPI").tag(75); Text("210 BPI").tag(210)
                }.labelsHidden().frame(width: 105)
            }
            TextField(index == 0 ? "MIDWEST TEST 12345" : "1234567890=12345", text: $model.drafts[index], axis: .vertical)
                .font(.system(size: 13, design: .monospaced)).lineLimit(2...4)
                .textFieldStyle(.plain).padding(12).background(canvas).cornerRadius(7)
                .accessibilityLabel("Track \(index + 1) data")
            HStack {
                Text(index == 0 ? "Accepts payload or %payload?" : "Accepts payload or ;payload?").font(.system(size: 10)).foregroundColor(.secondary)
                Spacer()
                Text("\(count) / \(limit) characters").font(.system(size: 10, design: .monospaced)).foregroundColor(count > limit ? .orange : .secondary)
            }
        }.padding(16).background(AppTheme.surface).cornerRadius(12).overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.primary.opacity(0.08)))
            .disabled(model.busy)
    }
    private var reader: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                Text("Read using").font(.system(size: 12, weight: .medium))
                Picker("Read using", selection: $model.readerMode) {
                    ForEach(AppModel.ReaderMode.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }.labelsHidden().frame(width: 240).disabled(model.busy)
                Spacer()
            }
            if model.readerMode == .keyboard {
                VStack(spacing: 16) {
                    Image(systemName: "waveform.path").font(.system(size: 42, weight: .light)).foregroundColor(teal)
                    Text(model.keyboardArmed ? "Ready for your swipe" : "MSR600 keyboard reader").font(.system(size: 21, weight: .semibold))
                    Text("Connect the reader, click Start reading, and swipe once.\nKeep this window active until all tracks appear.")
                        .font(.system(size: 12)).foregroundColor(.secondary).multilineTextAlignment(.center).lineSpacing(5)
                    HStack {
                        Button(model.keyboardArmed ? "Stop reading" : "Start reading") {
                            if model.keyboardArmed { model.stopKeyboard() } else { model.armKeyboard() }
                        }.buttonStyle(.borderedProminent).controlSize(.large).disabled(model.busy)
                        Button("Paste swipe") {
                            if let text = NSPasteboard.general.string(forType: .string) {
                                model.armKeyboard(); model.acceptKeyboard(text)
                            }
                        }.disabled(model.busy)
                    }
                    HStack {
                        Text("Input format").font(.system(size: 11, weight: .medium))
                        Picker("Input format", selection: $model.inputFormat) {
                            ForEach(SwipeInputFormat.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                        }.labelsHidden().frame(width: 260).disabled(model.keyboardArmed)
                            .onChange(of: model.inputFormat) { _ in model.reparseSwipe() }
                    }
                    Text("Sentinels are optional. For unmarked data, choose the track order.\nMultiple fields can be separated by Enter or Tab.")
                        .font(.system(size: 10)).foregroundColor(.secondary).multilineTextAlignment(.center)
                    KeyboardCapture(armed: model.keyboardArmed, generation: model.captureGeneration, onSwipe: model.acceptKeyboard, onError: model.keyboardError)
                        .frame(width: 1, height: 1).accessibilityLabel("Swipe input")
                }.frame(maxWidth: .infinity).padding(32).background(AppTheme.surface).cornerRadius(12)
                if !model.rawSwipe.isEmpty {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Text("RAW SWIPE").font(.system(size: 10, weight: .semibold)).tracking(1.4)
                            Spacer()
                            Text("\(model.rawSwipe.utf8.count) bytes").font(.system(size: 10, design: .monospaced)).foregroundColor(.secondary)
                        }
                        Text(KeyboardSwipeParser.visibleRaw(model.rawSwipe)).font(.system(size: 12, design: .monospaced))
                            .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .leading).padding(12).background(canvas).cornerRadius(7)
                        Text("Changing Input format reprocesses this swipe immediately. Raw input is retained even when track assignment is unknown.")
                            .font(.system(size: 10)).foregroundColor(.secondary)
                    }.padding(20).background(AppTheme.surface).cornerRadius(12)
                }
            } else {
                VStack(alignment: .leading, spacing: 14) {
                    Text("Read through the connected writer").font(.system(size: 18, weight: .semibold))
                    Text("Connect the X6 by USB/Bluetooth or the MSR605 by Serial in the sidebar. The density settings from Magstripe Write are used for this read.")
                        .font(.system(size: 12)).foregroundColor(.secondary)
                    Button("Read card") { model.readUSB() }.buttonStyle(.borderedProminent).controlSize(.large).disabled(!model.connected || model.busy)
                }.padding(24).frame(maxWidth: .infinity, alignment: .leading).background(AppTheme.surface).cornerRadius(12)
            }
            if model.readCard != nil { results }
        }
    }
    private var results: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("READ RESULTS").font(.system(size: 10, weight: .semibold)).tracking(1.4)
                Spacer()
                Button("Copy") { model.copyResults() }.controlSize(.small)
                Button("Use in editor") { model.useReadData() }.controlSize(.small).disabled(model.busy)
            }
            if let card = model.readCard {
                ForEach(0..<3, id: \.self) { index in
                    VStack(alignment: .leading, spacing: 7) {
                        HStack {
                            Text("Track \(index + 1)").font(.system(size: 12, weight: .semibold))
                            Spacer()
                            if model.verification.count == 3 {
                                if let match = model.verification[index] {
                                    Label(match ? "Matches" : "Mismatch", systemImage: match ? "checkmark.circle.fill" : "xmark.circle.fill")
                                        .font(.system(size: 11)).foregroundColor(match ? teal : .orange)
                                } else { Text("Not requested").font(.system(size: 11)).foregroundColor(.secondary) }
                            }
                        }
                        Text(card.tracks[index].isEmpty ? "No data" : card.framed(index))
                            .font(.system(size: 12, design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                            .fixedSize(horizontal: false, vertical: true).padding(10).background(canvas).cornerRadius(6)
                    }
                }
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
    private var activity: some View {
        DisclosureGroup("Session activity") {
            VStack(alignment: .leading, spacing: 8) {
                if model.log.isEmpty { Text("Connection and operation results appear here.").foregroundColor(.secondary) }
                ForEach(Array(model.log.enumerated()), id: \.offset) { _, entry in Text(entry).textSelection(.enabled) }
            }.font(.system(size: 10, design: .monospaced)).frame(maxWidth: .infinity, alignment: .leading).padding(.top, 12)
        }.font(.system(size: 11)).foregroundColor(.secondary).padding(.top, 4)
    }
    private var help: some View {
        VStack(alignment: .leading, spacing: 22) {
            helpItem("01", "Connect the writer", "Choose USB/Bluetooth for the X6, or Serial at 9600 baud for an MSR605. For USB, connect a data cable and Refresh. For Bluetooth, turn on the X6 and Scan. Close EasyMSR and other reader apps, then Connect. The app checks the device response before enabling writing.")
            helpItem("02", "Prepare and write", "Choose HiCo for your HiCo blanks. Standard ISO densities are 210 / 75 / 210 BPI. The short test preset also fits 75 BPI on all tracks. The app accepts outer sentinels and removes them before sending the payload to the writer.")
            helpItem("03", "Verify with a second swipe", "Click Write & verify. Swipe once to write, then wait for the prompt and swipe the same card again. A successful write response alone is not shown as a verified card.")
            helpItem("04", "Use the MSR600 reader", "Use its USB keyboard mode. Sentinels are optional: choose the track order under Input format when your reader omits them. On Magstripe Read, choose MSR600 and click Start reading. Track 3 may start with + or a second semicolon. Use a US keyboard input source if punctuation comes through incorrectly.")
            helpItem("05", "Read and write NFC records", "Choose HF Workspace and place your Classic 1K card on the ACS reader. Read card handles standard NFC keys automatically and shows URLs, text and other records. Under Write records, add records or use the current ones, then Write records & verify. The card must already be NFC-formatted. UID and keys stay unchanged.")
            helpItem("06", "HF tools and advanced settings", "Tools contains ACR122U/1552U reader settings and buzzer controls, plus Save credentials, Write from file and Copy to infinity for data-block profiles. Advanced contains custom keys and the block editor. Gen1a cards use normal authenticated Classic mode; UID changes and key changes are not included.")
            helpItem("07", "Piswords UHF tags", "Open UHF Workspace, connect the reader and scan. Stop scanning, select one tag, then read memory or review an EPC change. The new EPC is verified by reading it back. Reader tools includes buzzer and RF power controls.")
            helpItem("08", "Chameleon Ultra LF cards", "Connect over USB or Bluetooth using Chameleon Ultra in the sidebar, then open LF Workspace. Scan a source card, then remove it and place one T5577 target to clone. Default and recovery tools restore an EM410x test ID. Password writes are disabled. Writing requires firmware with open block-write support; unsupported formats stay read-only. Each write is checked by reading the card back.")
            Text("Copyright © 2026 Midwest Gadgets. Code: GNU GPL version 3. You may redistribute and modify it under GPLv3; there is no warranty. Branding has separate terms.").font(.caption).foregroundColor(.secondary)
            HStack {
                if let license = Bundle.main.url(forResource:"LICENSE",withExtension:"txt") { Button("View GPLv3 license") { NSWorkspace.shared.open(license) } }
                if let source = Bundle.main.url(forResource:"Source",withExtension:"zip") { Button("Show included source") { NSWorkspace.shared.activateFileViewerSelecting([source]) } }
            }
            Text("macOS 13 or later · Apple silicon and Intel · No runtime dependencies")
                .font(.system(size: 11)).foregroundColor(.secondary)
        }.padding(24).background(AppTheme.surface).cornerRadius(12)
    }
    private func helpItem(_ number: String, _ title: String, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 16) {
            Text(number).font(.system(size: 14, weight: .bold, design: .monospaced)).foregroundColor(teal)
            VStack(alignment: .leading, spacing: 7) {
                Text(title).font(.system(size: 15, weight: .semibold))
                Text(text).font(.system(size: 12)).foregroundColor(.secondary).lineSpacing(4)
            }
        }
    }
}
