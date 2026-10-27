// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import MagStripeCore

struct NFCView: View {
    @ObservedObject var model: NFCModel
    private let accent = Color(red: 0, green: 0.43, blue: 0.40)
    private let field = Color(red: 0.96, green: 0.97, blue: 0.98)
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
                if !model.selectedReader.hasPrefix("Chameleon Ultra · ") { ReaderConfigurationView(model:model) }
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
                Text("Connect an ACS reader, or connect your Chameleon in LF Workspace, then refresh.").font(.system(size: 12)).foregroundColor(.secondary)
            } else {
                Picker("Reader", selection: $model.selectedReader) {
                    ForEach(model.readers, id: \.self) { Text($0).tag($0) }
                }.onChange(of: model.selectedReader) { _ in model.readerChanged() }
            }
            Text("Chameleon Ultra shares its USB or Bluetooth connection from LF Workspace. Connect there, then refresh and select it here.").font(.caption).foregroundColor(.secondary)
            HStack {
                Text(model.automaticKeys ? "Place a card on the reader. Standard NFC keys are handled automatically." : "Using the custom key from Advanced. Turn on automatic keys there for standard NFC cards.")
                    .font(.system(size: 11)).foregroundColor(.secondary)
                Spacer()
                Button("Read card") { model.readRecords() }.buttonStyle(.borderedProminent).disabled(model.selectedReader.isEmpty)
            }
        }.padding(20).background(.white).cornerRadius(12).disabled(model.busy)
    }
    private var authentication: some View {
        VStack(alignment: .leading, spacing: 14) {
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
            if let snapshot = model.snapshot {
                Text(snapshot.card.atqa.map { "ATQA: " + HexBytes.string($0) + " · SAK: " + String(format:"%02X",snapshot.card.sak ?? 0) } ?? ("ATR: " + HexBytes.string(snapshot.card.atr))).font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
            }
            Text("Keys are tried only from the standard NFC/MAD keys, factory key and the key you enter. No key recovery is performed.").font(.system(size: 10)).foregroundColor(.secondary)
        }.padding(20).background(.white).cornerRadius(12).disabled(model.busy)
    }

    private var memory: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("CARD MEMORY").font(.system(size: 10, weight: .semibold)).tracking(1.4)
                Spacer()
                Text("16 sectors · 64 blocks · 1,024 bytes").font(.system(size: 10)).foregroundColor(.secondary)
            }
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 8), spacing: 6) {
                ForEach(0..<16, id: \.self) { sector in
                    Button { model.selectSector(sector) } label: {
                        VStack(spacing: 3) {
                            Text("Sector \(sector)").font(.system(size: 10, weight: .medium))
                            let read = (sector * 4..<sector * 4 + 4).filter { model.snapshot?.blocks[$0] != nil }.count
                            Text("\(read) / 4 read").font(.system(size: 9)).opacity(0.65)
                        }.frame(maxWidth: .infinity).padding(.vertical, 8)
                            .background(model.selectedSector == sector ? accent.opacity(0.13) : field).cornerRadius(6)
                            .overlay(RoundedRectangle(cornerRadius: 6).stroke(model.selectedSector == sector ? accent : .clear))
                    }.buttonStyle(.plain)
                }
            }
            ForEach(model.selectedSector * 4..<model.selectedSector * 4 + 4, id: \.self) { block in
                Button { model.selectBlock(block) } label: {
                    HStack(alignment: .top, spacing: 14) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Block \(block)").font(.system(size: 11, weight: .semibold))
                            Text(block == 0 ? "Manufacturer" : block % 4 == 3 ? "Sector trailer" : "Data").font(.system(size: 9)).foregroundColor(.secondary)
                        }.frame(width: 80, alignment: .leading)
                        VStack(alignment: .leading, spacing: 4) {
                            if let bytes = model.snapshot?.blocks[block] {
                                Text(HexBytes.string(bytes)).font(.system(size: 11, design: .monospaced))
                                Text(HexBytes.ascii(bytes)).font(.system(size: 10, design: .monospaced)).foregroundColor(.secondary)
                            } else {
                                Text(model.snapshot?.errors[block] ?? "Not read yet").font(.system(size: 11)).foregroundColor(.secondary)
                            }
                        }.frame(maxWidth: .infinity, alignment: .leading)
                        Image(systemName: ClassicClient.isDataBlock(block) ? "pencil" : "lock").font(.system(size: 11)).foregroundColor(.secondary)
                    }.padding(11).background(model.selectedBlock == block ? accent.opacity(0.06) : field).cornerRadius(6)
                }.buttonStyle(.plain)
            }
            Text("Keys in sector trailers are masked or restricted by the card. These read results are not a complete key backup.")
                .font(.system(size: 10)).foregroundColor(.secondary)
        }.padding(20).background(.white).cornerRadius(12).disabled(model.busy)
    }
    private var editor: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Edit block \(model.selectedBlock)").font(.system(size: 15, weight: .semibold))
                Spacer()
                Button("Test data") {
                    model.draft = HexBytes.string(Array("MIDWEST TEST 001".utf8))
                }.controlSize(.small).disabled(!ClassicClient.isDataBlock(model.selectedBlock))
            }
            if ClassicClient.isDataBlock(model.selectedBlock) {
                TextField("16 bytes in hexadecimal", text: $model.draft, axis: .vertical)
                    .font(.system(size: 13, design: .monospaced)).lineLimit(2...3).textFieldStyle(.plain).padding(12).background(field).cornerRadius(7)
                HStack {
                    Text(model.snapshot?.blocks[model.selectedBlock] == nil ? "Read this block before editing and writing." : model.draftError ?? "Exactly 16 bytes. Writing replaces this data block.")
                        .font(.system(size: 11)).foregroundColor(.secondary)
                    Spacer()
                    Button("Write block & verify") { model.write() }.buttonStyle(.borderedProminent).disabled(!model.canWrite)
                }
            } else {
                Text("Manufacturer data and sector trailers are read-only here. Gen1a UID-changing commands are not included.")
                    .font(.system(size: 12)).foregroundColor(.secondary)
            }
        }.padding(20).background(.white).cornerRadius(12).disabled(model.busy)
    }
}
