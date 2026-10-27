// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit
import MagStripeCore

private let hfAccent = Color(red: 0, green: 0.43, blue: 0.40)

struct NFCRecordsReadView: View {
    @ObservedObject var model: NFCModel
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            if let snapshot = model.snapshot {
                VStack(alignment: .leading, spacing: 18) {
                    HStack {
                        Image(systemName: "wave.3.right.circle.fill").font(.system(size: 32)).foregroundColor(hfAccent)
                        VStack(alignment: .leading, spacing: 5) {
                            Text(snapshot.card.name).font(.system(size: 19, weight: .semibold))
                            Text(snapshot.card.isClassic1K ? (model.ndef == nil ? "NFC-A · MIFARE Classic" : "NFC-A · MIFARE Classic · NDEF") : "Card identification only").font(.system(size: 12)).foregroundColor(.secondary)
                        }
                        Spacer()
                    }
                    Grid(alignment: .leading, horizontalSpacing: 30, verticalSpacing: 12) {
                        GridRow { Text("Card ID").foregroundColor(.secondary); Text(HexBytes.string(snapshot.card.uid)).textSelection(.enabled) }
                        if snapshot.card.isClassic1K { GridRow { Text("Memory").foregroundColor(.secondary); Text("1 KB · 16 sectors") } }
                        if let ndef = model.ndef {
                            GridRow { Text("Record size").foregroundColor(.secondary); Text("\(ndef.messageSize) / \(ndef.capacity) bytes") }
                            GridRow { Text("Writable").foregroundColor(.secondary); Text(ndef.writable ? "Yes" : "Not with the available key") }
                        }
                    }.font(.system(size: 12))
                }.padding(22).frame(maxWidth: .infinity, alignment: .leading).background(.white).cornerRadius(12)
                if let ndef = model.ndef {
                    HStack {
                        Text("\(ndef.records.count) NFC RECORD\(ndef.records.count == 1 ? "" : "S")").font(.system(size: 10, weight: .semibold)).tracking(1.3)
                        Spacer()
                        Button("Use records in writer") { model.useExistingRecords() }.disabled(model.busy)
                    }
                    if ndef.records.isEmpty { Text("This card is ready for records. Choose Write records to add one.").font(.system(size: 13)).foregroundColor(.secondary) }
                    ForEach(Array(ndef.records.enumerated()), id: \.element.id) { index, record in
                        NFCRecordCard(record: record, number: index + 1)
                    }
                } else {
                    Label(model.ndefMessage.isEmpty ? "Read the card to inspect its NFC records." : model.ndefMessage, systemImage: "info.circle")
                        .font(.system(size: 12)).foregroundColor(.secondary).padding(18).frame(maxWidth: .infinity, alignment: .leading).background(.white).cornerRadius(10)
                }
            } else {
                VStack(spacing: 16) {
                    Image(systemName: "sensor.tag.radiowaves.forward").font(.system(size: 44, weight: .light)).foregroundColor(hfAccent)
                    Text("Read a card's records").font(.system(size: 21, weight: .semibold))
                    Text("Place a card on your reader and click Read card.\nURLs, text and other NFC records appear here.").font(.system(size: 13)).foregroundColor(.secondary).multilineTextAlignment(.center)
                }.padding(48).frame(maxWidth: .infinity).background(.white).cornerRadius(12)
            }
        }
    }
}

struct NFCRecordCard: View {
    let record: NDEFRecord
    let number: Int
    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: record.uriValue == nil ? "doc.text" : "link").font(.system(size: 24)).foregroundColor(hfAccent).frame(width: 34)
            VStack(alignment: .leading, spacing: 8) {
                Text("Record \(number) · \(record.title)").font(.system(size: 14, weight: .semibold))
                Text(record.displayValue).font(.system(size: 13)).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                Text("\(record.payload.count) payload bytes").font(.system(size: 10)).foregroundColor(.secondary)
            }.frame(maxWidth: .infinity, alignment: .leading)
            Button("Copy") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(record.displayValue, forType: .string) }.controlSize(.small)
        }.padding(20).background(.white).cornerRadius(12)
    }
}

private struct RecordEditRequest: Identifiable {
    let id = UUID()
    var index: Int?
    var record: NDEFRecord?
}
struct NFCRecordsWriteView: View {
    @ObservedObject var model: NFCModel
    @State private var editor: RecordEditRequest?
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                VStack(alignment: .leading, spacing: 5) {
                    Text("Records to write").font(.system(size: 20, weight: .semibold))
                    Text("\(model.recordQueue.count) records · \(model.recordBytes) bytes\(model.ndef.map { " / \($0.capacity) available" } ?? "")")
                        .font(.system(size: 12)).foregroundColor(.secondary)
                }
                Spacer()
                Button("Use existing") { model.useExistingRecords() }.disabled(model.ndef == nil)
                Button { editor = RecordEditRequest() } label: { Label("Add record", systemImage: "plus") }.buttonStyle(.borderedProminent)
            }.disabled(model.busy)
            if model.recordQueue.isEmpty {
                VStack(alignment: .leading, spacing: 10) {
                    Text("Choose a record type to get started.").font(.system(size: 16, weight: .medium))
                    Text("Text, links, contact details, Wi-Fi, Bluetooth, filament information and custom data.").font(.system(size: 12)).foregroundColor(.secondary)
                }.padding(24).frame(maxWidth: .infinity, alignment: .leading).background(.white).cornerRadius(12)
            }
            ForEach(Array(model.recordQueue.enumerated()), id: \.element.id) { index, record in
                VStack(spacing: 0) {
                    NFCRecordCard(record: record, number: index + 1)
                    HStack {
                        Button("Edit") { editor = RecordEditRequest(index: index, record: record) }
                            .disabled(record.textValue == nil && record.uriValue == nil && record.tnf != 2 && !(record.tnf == 4 && record.typeName == "android.com:pkg"))
                        Button("Move up") { model.recordQueue.swapAt(index, index - 1) }.disabled(index == 0)
                        Spacer()
                        Button("Remove") { model.recordQueue.remove(at: index) }
                    }.controlSize(.small).padding(.horizontal, 20).padding(.bottom, 16).disabled(model.busy)
                }.background(.white).cornerRadius(12)
            }
            HStack(alignment: .center, spacing: 18) {
                VStack(alignment: .leading, spacing: 5) {
                    Text(model.recordWriteError ?? "Ready to write and verify.").font(.system(size: 12, weight: .medium))
                    Text("Writing replaces the card's NDEF message. Use existing to keep its current records. UID and keys stay unchanged.")
                        .font(.system(size: 11)).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                Button("Write records & verify") { model.writeRecords() }.buttonStyle(.borderedProminent).controlSize(.large)
                    .disabled(model.busy || model.recordWriteError != nil)
            }.padding(20).background(.white).cornerRadius(12)
        }
        .sheet(item: $editor) { request in
            NFCRecordEditor(record: request.record) { record in
                if let index = request.index, model.recordQueue.indices.contains(index) { model.recordQueue[index] = record }
                else { model.recordQueue.append(record) }
            }
        }
    }
}

struct NFCRecordEditor: View {
    @Environment(\.dismiss) private var dismiss
    @State private var kind: RecordKind
    @State private var values: [String: String]
    private let originalIdentifier: [UInt8]
    let save: (NDEFRecord) -> Void
    init(record: NDEFRecord? = nil, initialKind: RecordKind = .url, save: @escaping (NDEFRecord) -> Void) {
        self.save = save
        originalIdentifier = record?.identifier ?? []
        var kind = initialKind, values: [String: String] = [:]
        if let record {
            if let text = record.textValue {
                kind = .text
                let count = Int((record.payload.first ?? 0) & 0x3F)
                values = ["text":text, "language":String(bytes: record.payload.dropFirst().prefix(count), encoding: .ascii) ?? "en"]
            } else if let uri = record.uriValue { kind = uri.hasPrefix("https://") || uri.hasPrefix("http://") ? .url : .customURI; values = ["url":uri] }
            else if record.tnf == 4, record.typeName == "android.com:pkg" { kind = .application; values = ["package":String(decoding: record.payload, as: UTF8.self)] }
            else { kind = .data; values = ["mime":record.tnf == 2 ? record.typeName : "application/octet-stream", "encoding":"Hexadecimal", "payload":HexBytes.string(record.payload)] }
        }
        _kind = State(initialValue: kind); _values = State(initialValue: kind.defaults.merging(values) { _, new in new })
    }
    private var result: Result<NDEFRecord, Error> { Result {
        var record = try RecordBuilder.make(kind, values: values)
        record.identifier = originalIdentifier
        return record
    } }
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("NFC record").font(.system(size: 24, weight: .semibold))
            Picker("Record type", selection: $kind) {
                ForEach(RecordKind.allCases) { Text($0.rawValue).tag($0) }
            }.onChange(of: kind) { kind in values = kind.defaults }
            ScrollView {
                VStack(alignment: .leading, spacing: 15) {
                    ForEach(kind.fields) { field in
                        VStack(alignment: .leading, spacing: 6) {
                            Text(field.title).font(.system(size: 12, weight: .medium))
                            let binding = Binding<String>(get: { values[field.id] ?? "" }, set: { values[field.id] = $0 })
                            if !field.options.isEmpty {
                                Picker(field.title, selection: binding) { ForEach(field.options, id: \.self) { Text($0).tag($0) } }.labelsHidden()
                            } else if field.secret {
                                SecureField(field.placeholder, text: binding).textFieldStyle(.roundedBorder).frame(minHeight: 24)
                            } else {
                                TextField(field.placeholder, text: binding, axis: .vertical).lineLimit(1...5).textFieldStyle(.roundedBorder).frame(minHeight: 24)
                            }
                        }
                    }
                    Text(kind.hint).font(.system(size: 11)).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                }.padding(.trailing, 8)
            }
            HStack {
                switch result {
                case .success(let record): Text("\((try? NDEFCodec.encode([record]).count) ?? 0) bytes").font(.system(size: 11)).foregroundColor(.secondary)
                case .failure(let error): Text(error.localizedDescription).font(.system(size: 11)).foregroundColor(.secondary)
                }
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Save record") { if case .success(let record) = result { save(record); dismiss() } }
                    .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                    .disabled({ if case .failure = result { return true }; return false }())
            }
        }.padding(28).frame(width: 560, height: 590).background(Color(nsColor: .windowBackgroundColor)).preferredColorScheme(.light)
    }
}
