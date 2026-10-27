// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import MagStripeCore

struct LFView: View {
    @ObservedObject var model: LFModel
    private let accent = AppTheme.accent
    var body: some View {
        VStack(alignment:.leading,spacing:20) {
            HStack(spacing:12) {
                if model.busy { ProgressView().controlSize(.small) } else { Image(systemName:model.statusKind == .error ? "exclamationmark.circle" : "waveform.path") }
                Text(model.status).font(.system(size:12,weight:.medium)).fixedSize(horizontal:false,vertical:true)
                Spacer(minLength:0)
                if model.busy && !model.hfWorkflowBusy { Button("Cancel") { model.cancel() } }
            }.padding(14).background((model.statusKind == .error ? Color.orange : accent).opacity(0.07)).cornerRadius(10)
            Text("Connect Chameleon Ultra in the sidebar. The same USB or Bluetooth connection serves HF and LF workspaces.")
                .font(.callout).foregroundColor(.secondary)
            cardReader
            tools
            if !model.log.isEmpty {
                DisclosureGroup("Activity") {
                    VStack(alignment:.leading,spacing:6) { ForEach(Array(model.log.prefix(12).enumerated()),id:\.offset) { _,text in Text(text).font(.system(size:10)).textSelection(.enabled) } }.frame(maxWidth:.infinity,alignment:.leading).padding(.top,8)
                }.padding(18).background(AppTheme.surface).cornerRadius(12)
            }
        }
        .sheet(item:$model.preview) { intent in review(intent) }
    }
    private var cardReader: some View {
        VStack(alignment:.leading,spacing:14) {
            HStack {
                Text("READ & CLONE").font(.system(size:11,weight:.semibold)).tracking(1)
                Spacer(); Button("Open LF file") { model.load() }.disabled(model.busy)
                Button("Save card") { model.save() }.disabled(model.card == nil || model.busy)
            }
            HStack {
                Picker("Card type",selection:$model.format) {
                    Text("Detect automatically").tag(nil as LFFormat?)
                    ForEach(model.info?.formats ?? []) { Text($0.rawValue).tag(Optional($0)) }
                }.frame(maxWidth:300).disabled(model.busy)
                if model.format == .hid {
                    Picker("HID format",selection:$model.hidHint) {
                        Text("Automatic").tag(0); Text("H10301 · 26-bit").tag(1); Text("H10306 · 34-bit").tag(16); Text("H10302 · 37-bit").tag(26); Text("H10304 · 37-bit").tag(27)
                    }.disabled(model.busy)
                }
                Spacer()
                Button("Scan & read") { model.scan() }.buttonStyle(.borderedProminent).disabled(!model.canOperate)
            }
            if let card = model.card {
                VStack(alignment:.leading,spacing:8) {
                    Text(card.format.rawValue).font(.headline)
                    Text(card.identifier).font(.system(size:15,weight:.medium,design:.monospaced)).textSelection(.enabled).fixedSize(horizontal:false,vertical:true)
                    Text(card.details).font(.caption).foregroundColor(.secondary)
                }.frame(maxWidth:.infinity,alignment:.leading).padding(14).background(accent.opacity(0.06)).cornerRadius(8)
                HStack {
                    Text("Remove the source and place one writable T5577 target on the LF antenna.").font(.caption).foregroundColor(.secondary)
                    Spacer(); Button("Clone card…") { model.prepare(.clone) }.disabled(!model.canClone)
                }
            } else { Text("Place one LF card on the antenna to read its ID. A saved scan can be cloned to a T5577 tag.").font(.callout).foregroundColor(.secondary) }
            Text("Reads the card’s transmitted ID and format. Full T5577 memory dumps are not exposed by the supported firmware.").font(.caption).foregroundColor(.secondary)
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
    private var tools: some View {
        VStack(alignment:.leading,spacing:14) {
            Label("T5577 TOOLS",systemImage:"wrench.and.screwdriver").font(.system(size:11,weight:.semibold))
            HStack {
                Text("Default test ID").font(.callout)
                TextField("0102030405",text:$model.defaultID).textFieldStyle(.roundedBorder).font(.system(size:13,design:.monospaced)).frame(width:170)
                Text("EM410x · 10 hex digits").font(.caption).foregroundColor(.secondary)
                Spacer(); Button("Write default…") { model.prepare(.defaults) }.disabled(!model.canWriteDefault)
            }.disabled(model.busy)
            HStack(alignment:.top) {
                VStack(alignment:.leading,spacing:5) {
                    Text("Recover an unreadable T5577").font(.system(size:13,weight:.medium))
                    Text("Rewrites its configuration and test ID using open block writes. This replaces existing data; it does not retrieve lost data. Password-protected cards, locked blocks, unsupported downlink modes, and damaged tags cannot be recovered by this workflow.").font(.caption).foregroundColor(.secondary).fixedSize(horizontal:false,vertical:true)
                }
                Spacer(); Button("Recover T5577…") { model.prepare(.recover) }.disabled(!model.canWriteDefault)
            }
            Text("Password writes are disabled. The app uses open block writes, leaves the password block untouched, and keeps password protection off. Requires firmware with command 3016. EM410x, Electra, Viking, ioProx, PAC and Jablotron are supported; HID/Pyramid remain read-only.").font(.caption).foregroundColor(.secondary)
            if model.connected && !model.canWriteDefault && !model.busy {
                Text("This firmware does not expose password-free T5577 writing. Scanning and HF operations remain available.").font(.caption).foregroundColor(.orange)
            }
        }.padding(20).background(AppTheme.surface).cornerRadius(12)
    }
    private func review(_ intent: LFWriteIntent) -> some View {
        VStack(alignment:.leading,spacing:16) {
            Text(intent.title).font(.title2.weight(.semibold))
            Text("Remove the source card. Keep only the intended T5577 target on the LF antenna.").font(.callout)
            Text("\(intent.card.format.rawValue) · \(intent.card.identifier)").font(.system(size:14,weight:.medium,design:.monospaced)).textSelection(.enabled).fixedSize(horizontal:false,vertical:true)
            Text("Replaces the target’s ID and configuration. No password is written, the password block is untouched, and password protection stays off. Already protected cards are not supported.").font(.callout)
            Text("The app reads the card back before reporting success. Cancelling after writing starts cannot undo the change.").font(.callout).foregroundColor(.secondary)
            if intent.action == .recover { Text("Recovery is an attempt to restore a readable EM410x configuration. It cannot repair permanently locked or damaged tags.").font(.callout).foregroundColor(.secondary) }
            HStack { Button("Cancel") { model.preview = nil }; Spacer(); Button("Write & verify") { model.write(intent) }.buttonStyle(.borderedProminent) }
        }.padding(28).frame(width:560)
    }
}
