// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import MagStripeCore

struct UHFView: View {
    @ObservedObject var model: UHFModel
    private let accent = AppTheme.accent
    var body: some View {
        VStack(alignment:.leading,spacing:20) {
            HStack(spacing:12) {
                if model.busy || model.scanning { ProgressView().controlSize(.small) }
                else { Image(systemName:model.statusKind == .error ? "exclamationmark.circle" : "antenna.radiowaves.left.and.right") }
                Text(model.status).font(.system(size:12,weight:.medium)).fixedSize(horizontal:false,vertical:true)
                Spacer(minLength:0)
            }.padding(14).background((model.statusKind == .error ? Color.orange : accent).opacity(0.07)).cornerRadius(10)
            connection
            inventory
            if let tag = model.tag {
                epcEditor(tag)
                memoryEditor
            }
            if model.connected { readerTools }
            if !model.log.isEmpty {
                DisclosureGroup("Activity") {
                    VStack(alignment:.leading,spacing:6) { ForEach(Array(model.log.prefix(12).enumerated()),id:\.offset) { _,text in Text(text).font(.system(size:10)).textSelection(.enabled) } }.frame(maxWidth:.infinity,alignment:.leading).padding(.top,8)
                }.padding(18).background(AppTheme.surface).cornerRadius(12)
            }
        }
        .onAppear { if model.autoDiscovery && model.ports.isEmpty && !model.busy { model.refresh() } }
        .sheet(item:$model.writePreview) { intent in
            VStack(alignment:.leading,spacing:18) {
                Text(intent.isEPC ? "Write new EPC" : "Write User memory").font(.title2.weight(.semibold))
                Text("Keep only the intended tag near the reader. Tags sharing the same EPC cannot be distinguished by this selection.").font(.callout)
                Text("Current EPC").font(.caption).foregroundColor(.secondary)
                Text(HexBytes.string(intent.oldEPC)).font(.system(size:12,design:.monospaced)).textSelection(.enabled)
                Text(intent.isEPC ? "New EPC" : "\(intent.bytes.count) bytes at User word \(intent.word)").font(.caption).foregroundColor(.secondary)
                Text(HexBytes.string(intent.bytes)).font(.system(size:12,design:.monospaced)).textSelection(.enabled).fixedSize(horizontal:false,vertical:true)
                Text(intent.isEPC ? "The EPC length is updated automatically. The app then reads back and verifies the new EPC." : "These bytes replace the selected User-memory words. The app then reads them back to verify.").font(.callout)
                HStack { Button("Cancel") { model.writePreview = nil }; Spacer(); Button("Write & verify") { model.performWrite(intent) }.buttonStyle(.borderedProminent) }
            }.padding(28).frame(width:540)
        }
    }
    private var connection: some View {
        VStack(alignment:.leading,spacing:14) {
            HStack { Label("PISWORDS UHF READER",systemImage:"cable.connector").font(.system(size:11,weight:.semibold)); Spacer(); Button("Refresh") { model.refresh() }.disabled(model.connected || model.busy) }
            if model.ports.isEmpty { Text("Connect the Piswords USB reader, then refresh. USB serial connections are supported.").font(.callout).foregroundColor(.secondary) }
            else { Picker("Reader",selection:$model.selectedPort) { ForEach(model.ports) { Text($0.label).tag($0.path) } }.disabled(model.connected || model.busy) }
            HStack(spacing:16) {
                Picker("Baud rate",selection:$model.baud) {
                    Text("Detect automatically").tag(0)
                    ForEach([9600,19200,38400,57600,115200,230400],id:\.self) { Text(String($0)).tag($0) }
                }.frame(width:245).disabled(model.connected)
                TextField("Address",text:$model.address).textFieldStyle(.roundedBorder).frame(width:75).help("Reader address, usually 0").disabled(model.connected)
                Spacer()
                Button(model.connected ? "Disconnect" : "Connect") { if model.connected { model.disconnect() } else { model.connect() } }
                    .buttonStyle(.borderedProminent).disabled(model.busy || (!model.connected && model.selectedPort.isEmpty))
            }.disabled(model.busy)
            if !model.version.isEmpty { Text(model.version).font(.system(size:13,weight:.medium)); Text(model.details).font(.caption).foregroundColor(.secondary) }
            Text("UHF / EPC Gen2 tags use this workspace. NFC and MIFARE cards use HF Workspace.").font(.caption).foregroundColor(.secondary)
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
    private var inventory: some View {
        VStack(alignment:.leading,spacing:14) {
            HStack {
                Text("TAGS · \(model.tags.count)").font(.system(size:11,weight:.semibold)).tracking(1)
                Spacer()
                Button("Save scan") { model.exportScan() }.disabled(model.tags.isEmpty || model.busy)
                Button("Clear") { model.clear() }.disabled(model.busy || model.scanning)
                Button(model.scanning ? "Stop scanning" : "Scan tags") { if model.scanning { model.stop() } else { model.scan() } }
                    .buttonStyle(.borderedProminent).disabled(!model.connected || model.busy)
            }
            if model.tags.isEmpty { Text("Place a UHF tag near the reader, then scan. Select a result to read its memory or change its EPC.").font(.callout).foregroundColor(.secondary).padding(.vertical,12) }
            else {
                ScrollView {
                    LazyVStack(spacing:6) {
                        ForEach(model.tags) { tag in
                            Button { model.selectTag(tag.id) } label: {
                                HStack(alignment:.top) {
                                    VStack(alignment:.leading,spacing:5) {
                                        Text(tag.id).font(.system(size:12,weight:.medium,design:.monospaced)).fixedSize(horizontal:false,vertical:true)
                                        Text("Antenna \(tag.antenna) · \(String(format:"%.3f",Double(tag.frequencyKHz)/1000)) MHz" + (tag.signal.map { " · \($0) dBm" } ?? "")).font(.caption).foregroundColor(.secondary)
                                    }
                                    Spacer()
                                    Text("\(tag.sightings) reads").font(.caption).foregroundColor(.secondary)
                                    if model.selectedTag == tag.id { Image(systemName:"checkmark.circle.fill").foregroundColor(accent) }
                                }.padding(12).background(model.selectedTag == tag.id ? accent.opacity(0.08) : Color.gray.opacity(0.04)).cornerRadius(8)
                            }.buttonStyle(.plain).disabled(model.busy)
                        }
                    }
                }.frame(maxHeight:240)
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
    private func epcEditor(_ tag: UHFTag) -> some View {
        VStack(alignment:.leading,spacing:12) {
            Label("EDIT EPC",systemImage:"pencil.line").font(.system(size:11,weight:.semibold))
            Text("Current: \(tag.id)").font(.system(size:11,design:.monospaced)).textSelection(.enabled)
            TextField("New EPC in hexadecimal",text:$model.newEPC).textFieldStyle(.roundedBorder).font(.system(size:13,design:.monospaced))
            HStack {
                Text("2–62 bytes, in complete two-byte words. The tag must support the requested length.").font(.caption).foregroundColor(.secondary)
                Spacer()
                Button("Review EPC change") { model.prepareWrite(epc:true) }.buttonStyle(.borderedProminent)
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12).disabled(!model.canOperate)
    }
    private var memoryEditor: some View {
        VStack(alignment:.leading,spacing:14) {
            Text("TAG MEMORY").font(.system(size:11,weight:.semibold))
            HStack {
                Picker("Bank",selection:$model.bank) { ForEach(UHFMemoryBank.allCases,id:\.self) { Text($0.name).tag($0) } }.frame(maxWidth:280)
                TextField("Start word",text:$model.word).textFieldStyle(.roundedBorder).frame(width:85).help("Starting word, in decimal")
                TextField("Words",text:$model.count).textFieldStyle(.roundedBorder).frame(width:70).help("Number of two-byte words to read (1–64)")
                Spacer()
                Button("Read memory") { model.readMemory() }.buttonStyle(.borderedProminent)
            }.disabled(!model.canOperate)
            HStack {
                Text("Access password").font(.caption)
                SecureField("00000000",text:$model.password).textFieldStyle(.roundedBorder).frame(width:150).disabled(!model.canOperate)
                Text("8 hex digits · used for memory and EPC changes").font(.caption).foregroundColor(.secondary)
            }
            if let read = model.memory {
                Text(model.memoryLabel).font(.caption.weight(.medium))
                Text(HexBytes.string(read.bytes)).font(.system(size:12,design:.monospaced)).textSelection(.enabled).fixedSize(horizontal:false,vertical:true)
                Text(HexBytes.ascii(read.bytes)).font(.system(size:11,design:.monospaced)).foregroundColor(.secondary).textSelection(.enabled)
            }
            DisclosureGroup("Write User memory") {
                VStack(alignment:.leading,spacing:10) {
                    Text("Writes to the User bank at the starting word above. The EPC editor handles EPC changes separately.").font(.caption).foregroundColor(.secondary)
                    TextField("Hexadecimal bytes",text:$model.writeData).textFieldStyle(.roundedBorder).font(.system(size:12,design:.monospaced))
                    Button("Review User-memory write") { model.prepareWrite(epc:false) }
                }.padding(.top,10).disabled(!model.canOperate)
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
    private var readerTools: some View {
        DisclosureGroup("Reader tools") {
            VStack(alignment:.leading,spacing:14) {
                HStack { Text("Buzzer"); Spacer(); Button("Off") { model.setBuzzer(false) }; Button("On") { model.setBuzzer(true) } }
                HStack { Stepper("RF power: \(model.power) dBm",value:$model.power,in:0...30); Spacer(); Button("Apply power") { model.applyPower() } }
                Text("The reader reports unsupported power settings as an error. Its current frequency region is retained.").font(.caption).foregroundColor(.secondary)
            }.padding(.top,14).disabled(!model.canOperate)
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
}
