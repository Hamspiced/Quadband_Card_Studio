// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import MagStripeCore

struct NFCView: View {
    @ObservedObject var model: NFCModel
    var hub: CoreS3Hub? = nil
    private let accent = AppTheme.accent
    private let field = AppTheme.canvas
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(spacing: 12) {
                if model.busy { ProgressView().controlSize(.small) }
                else { Image(systemName: model.statusKind == .error ? "exclamationmark.circle" : "wave.3.right.circle").foregroundColor(model.statusKind == .error ? .orange : accent) }
                Text(model.status).font(.system(size: 12, weight: .medium)).fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
                if model.busy && !model.writing && !model.batchRunning { Button("Cancel") { model.cancel() }.controlSize(.small) }
            }.padding(14).background((model.statusKind == .error ? Color.orange : accent).opacity(0.07)).cornerRadius(10)
            connection
            Picker("HF section", selection: $model.hfPage) {
                ForEach(NFCModel.HFPage.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }.pickerStyle(.segmented).labelsHidden()
            switch model.hfPage {
            case .read: NFCRecordsReadView(model: model)
            case .write: NFCRecordsWriteView(model: model)
            case .tools:
                if !model.selectedReader.hasPrefix("Chameleon Ultra · ") && !model.selectedReader.hasPrefix("CoreS3 · ") { ReaderConfigurationView(model:model) }
                NFCToolkitView(model: model)
            case .advanced:
                authentication
                memory
                editor
            }

        }.onAppear { if model.autoDiscovery && !model.busy { model.refresh() } }
    }
    private var connection: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Label("NFC READER", systemImage: "sensor.tag.radiowaves.forward").font(.system(size: 11, weight: .semibold))
                Spacer()
                Button("Refresh readers") { model.refresh() }
            }
            if model.readers.isEmpty {
                Text("Connect an ACS reader, or connect your Chameleon in the sidebar, then refresh.").font(.system(size: 12)).foregroundColor(.secondary)
            } else {
                Picker("Reader", selection: $model.selectedReader) {
                    Text("Choose reader").tag("")
                    ForEach(model.readers, id: \.self) { Text($0).tag($0) }
                }.onChange(of: model.selectedReader) { value in
                    if !value.isEmpty { model.requiresExplicitReaderSelection = false }
                    model.readerChanged()
                }
            }
            Text("Chameleon and CoreS3 share their sidebar connections. Connect there, then refresh and explicitly select an HF reader. NTAG page operations use the Grove HF module, not a Chameleon raw-command tunnel.").font(.caption).foregroundColor(.secondary)
            HStack {
                Text(model.automaticKeys ? "Place a card on the reader. Standard NFC keys are handled automatically." : "Using the custom key from Advanced. Turn on automatic keys there for standard NFC cards.")
                    .font(.system(size: 11)).foregroundColor(.secondary)
                Spacer()
                Button("Read card") { model.readRecords() }.buttonStyle(.borderedProminent).disabled(model.selectedReader.isEmpty)
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12).disabled(model.busy)
    }
    private var authentication: some View {
        VStack(alignment: .leading, spacing: 14) {
            if model.selectedReader.hasPrefix("CoreS3 · "), let hub { CoreS3AuthenticationView(hub:hub,onChange:model.readerChanged) }
            if model.family.isNTAG {
                HStack {
                    Text("Type 2 memory uses four-byte pages. Reads include user pages and readable permission metadata, never password/PACK pages.").font(.caption).foregroundColor(.secondary)
                    Spacer()
                    Button("Identify only") { model.read(identifyOnly:true) }
                    Button("Read pages") { model.read() }.buttonStyle(.borderedProminent)
                }.disabled(model.selectedReader.isEmpty)
            } else {
            Toggle("Automatically use standard NFC and factory keys", isOn: $model.automaticKeys)
            HStack {
                Picker("Custom key", selection: $model.keyType) {
                    ForEach(ClassicKeyType.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }.frame(width: 170)
                TextField("Six-byte key", text: $model.key).font(.system(size: 12, design: .monospaced)).textFieldStyle(.roundedBorder)
                Button("Factory key") { model.key = "FF FF FF FF FF FF" }
            }
            HStack {
                Picker("Read", selection: $model.allSectors) { Text("All sectors").tag(true); Text("Selected sector").tag(false) }.frame(width: 220)
                Spacer()
                Button("Identify only") { model.read(identifyOnly: true) }
                Button("Read blocks") { model.read() }.buttonStyle(.borderedProminent)
            }.disabled(model.selectedReader.isEmpty)
            }
            if let snapshot = model.snapshot {
                Text(snapshot.card.atqa.map { "ATQA: " + HexBytes.string($0) + " · SAK: " + String(format:"%02X",snapshot.card.sak ?? 0) } ?? ("ATR: " + HexBytes.string(snapshot.card.atr))).font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
            }
            Text(model.family.isNTAG ? "Passwords are only used when explicitly supplied and supported by the reader; protection settings stay unchanged." : "Keys are tried only from the standard NFC/MAD keys, factory key and the key you enter. No key recovery is performed.").font(.system(size: 10)).foregroundColor(.secondary)
        }.padding(20).background(AppTheme.surface).cornerRadius(12).disabled(model.busy)
    }

    private var memory: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("CARD MEMORY").font(.system(size: 10, weight: .semibold)).tracking(1.4)
                Spacer()
                Text("\(model.family.isNTAG ? "\(model.family.unitCount) pages" : "\(model.family.sectorCount) sectors · \(model.family.unitCount) blocks") · \(model.family.unitCount * model.unitSize) bytes").font(.system(size: 10)).foregroundColor(.secondary)
            }
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 8), spacing: 6) {
                ForEach(model.memoryGroups, id: \.self) { sector in
                    Button { model.selectSector(sector) } label: {
                        VStack(spacing: 3) {
                            Text(model.family.isNTAG ? "Pages \(sector*16)+" : "Sector \(sector)").font(.system(size: 10, weight: .medium))
                            let units = model.units(in:sector)
                            let read = units.filter { model.snapshot?.blocks[$0] != nil }.count
                            Text("\(read) / \(units.count) read").font(.system(size: 9)).opacity(0.65)
                        }.frame(maxWidth: .infinity).padding(.vertical, 8)
                            .background(model.selectedSector == sector ? accent.opacity(0.13) : field).cornerRadius(6)
                            .overlay(RoundedRectangle(cornerRadius: 6).stroke(model.selectedSector == sector ? accent : .clear))
                    }.buttonStyle(.plain)
                }
            }
            ForEach(model.selectedUnits, id: \.self) { block in
                Button { model.selectBlock(block) } label: {
                    HStack(alignment: .top, spacing: 14) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("\(model.unitLabel.capitalized) \(block)").font(.system(size: 11, weight: .semibold))
                            Text(model.family.isData(block) ? "Data" : model.family.isNTAG ? "Protected metadata" : block == 0 ? "Manufacturer" : "Sector trailer").font(.system(size: 9)).foregroundColor(.secondary)
                        }.frame(width: 80, alignment: .leading)
                        VStack(alignment: .leading, spacing: 4) {
                            if let bytes = model.snapshot?.blocks[block] {
                                Text(HexBytes.string(bytes)).font(.system(size: 11, design: .monospaced))
                                Text(HexBytes.ascii(bytes)).font(.system(size: 10, design: .monospaced)).foregroundColor(.secondary)
                            } else {
                                Text(model.snapshot?.errors[block] ?? "Not read yet").font(.system(size: 11)).foregroundColor(.secondary)
                            }
                        }.frame(maxWidth: .infinity, alignment: .leading)
                        Image(systemName: model.family.isData(block) ? "pencil" : "lock").font(.system(size: 11)).foregroundColor(.secondary)
                    }.padding(11).background(model.selectedBlock == block ? accent.opacity(0.06) : field).cornerRadius(6)
                }.buttonStyle(.plain)
            }
            Text(model.family.isNTAG ? "UID, CC, locks, configuration and password/PACK are never writable. Password/PACK pages are not fetched. Unknown permissions remain read-only." : "Keys in sector trailers are masked or restricted by the card. These read results are not a complete key backup.")
                .font(.system(size: 10)).foregroundColor(.secondary)
        }.padding(20).background(AppTheme.surface).cornerRadius(12).disabled(model.busy)
    }
    private var editor: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Edit \(model.unitLabel) \(model.selectedBlock)").font(.system(size: 15, weight: .semibold))
                Spacer()
                Button("Test data") {
                    model.draft = HexBytes.string(Array("MIDWEST TEST 001!".utf8.prefix(model.unitSize)))
                }.controlSize(.small).disabled(!model.dataUnitSelected)
            }
            if model.dataUnitSelected {
                TextField("\(model.unitSize) bytes in hexadecimal", text: $model.draft, axis: .vertical)
                    .font(.system(size: 13, design: .monospaced)).lineLimit(2...3).textFieldStyle(.plain).padding(12).background(field).cornerRadius(7)
                HStack {
                    Text(model.snapshot?.blocks[model.selectedBlock] == nil ? "Read this unit before editing and writing." : model.draftError ?? model.selectedWriteRestriction ?? "Exactly \(model.unitSize) bytes. Writing replaces this data unit.")
                        .font(.system(size: 11)).foregroundColor(.secondary)
                    Spacer()
                    Button("Write \(model.unitLabel) & verify") { model.write() }.buttonStyle(.borderedProminent).disabled(!model.canWrite)
                }
            } else {
                Text("Manufacturer data and sector trailers are read-only here. Gen1a UID-changing commands are not included.")
                    .font(.system(size: 12)).foregroundColor(.secondary)
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12).disabled(model.busy)
    }
}
