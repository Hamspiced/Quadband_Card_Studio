// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import MagStripeCore

struct ReaderConfigurationView: View {
    @ObservedObject var model: NFCModel
    var body: some View {
        VStack(alignment:.leading,spacing:16) {
            HStack { Label("READER TOOLS",systemImage:"slider.horizontal.3").font(.system(size:11,weight:.semibold)); Spacer(); Button("Read reader settings") { model.readReaderSettings() }.disabled(model.busy || ReaderFamily.detect(model.selectedReader) == nil) }
            Text("ACR122U / ACR1552U · Load settings, choose changes, then apply. Some Mac drivers require a card on the ACR122U for these controls.").font(.caption).foregroundColor(.secondary)
            if let snapshot = model.readerSettings {
                Text(snapshot.firmware).font(.system(size:13,weight:.semibold))
                HStack { Text("Buzzer").font(.callout.weight(.medium)); Spacer(); Button("Buzzer off") { model.stageReaderPreset("mute") }; Button("Buzzer on") { model.stageReaderPreset("unmute") } }.disabled(model.busy)
                settingsGroup("Buzzer",snapshot)
                DisclosureGroup("RF & polling") { settingsGroup("RF & polling",snapshot).padding(.top,10) }
                if snapshot.family == .acr1552 {
                    HStack {
                        Menu("Community RF presets") {
                            Button("Tototo31 · 80%, 250 ms") { model.stageReaderPreset("tototo31") }
                            Button("Ashsh · 100%, continuous") { model.stageReaderPreset("ashsh") }
                            Button("Hardware power default · 250 ms") { model.stageReaderPreset("rfDefaults") }
                        }.frame(width:220)
                        Text("Stages settings from the supplied script.").font(.caption).foregroundColor(.secondary)
                    }.disabled(model.busy)
                } else {
                    Text("ACR122U supports polling, RF on/off and card-detection buzzer controls. It has no documented equivalent to the ACR1552U percentage-power setting.").font(.caption).foregroundColor(.secondary)
                }
                ForEach(["Card detection","Lights","USB & keyboard"],id:\.self) { group in
                    if ReaderSettingsCatalog.fields(snapshot.family).contains(where: { $0.group == group }) {
                        DisclosureGroup(group) { settingsGroup(group,snapshot).padding(.top,10) }
                    }
                }
                switch model.readerConfigPlan {
                case .success(let changes):
                    HStack { Text("\(changes.count) setting group\(changes.count == 1 ? "" : "s") staged").font(.caption).foregroundColor(.secondary); Spacer(); Button("Discard changes") { model.readerEdits = [:] }; Button("Review & apply") { model.readerConfigPreview = true }.buttonStyle(.borderedProminent).disabled(changes.isEmpty) }.disabled(model.busy)
                case .failure(let error): Text(error.localizedDescription).font(.caption).foregroundColor(.orange)
                }
            }
            ForEach(Array(model.readerConfigResults.enumerated()),id:\.offset) { _,result in
                VStack(alignment:.leading,spacing:5) {
                    Label(result.title,systemImage:result.success ? "checkmark.circle" : "exclamationmark.circle").font(.callout)
                    Text((result.verified ? "Verified. " : result.success ? "Acknowledged. " : "") + result.detail).font(.caption).foregroundColor(.secondary)
                }.frame(maxWidth:.infinity,alignment:.leading).padding(10).background((result.success ? Color.green : .orange).opacity(0.07)).cornerRadius(6)
            }
            Divider()
            HStack {
                Link("Tototo31 / Ashsh · original discussion",destination:URL(string:ReaderSettingsCatalog.sourceURL)!).font(.caption)
                Spacer()
                if Bundle.main.url(forResource:"acr1552u_rf_config",withExtension:"py") != nil {
                    Button("Save supplied Python script") { model.exportReaderScript() }.controlSize(.small)
                }
            }
            Text("The reader controls are built into this app; no Python setup is needed. The original reference script is not redistributed in public builds.").font(.caption).foregroundColor(.secondary)
        }.padding(20).background(.white).cornerRadius(12)
        .sheet(isPresented:$model.readerConfigPreview) {
            VStack(alignment:.leading,spacing:18) {
                Text("Apply reader settings").font(.title2.weight(.semibold))
                Text(model.selectedReader).font(.callout)
                ScrollView {
                    if case .success(let changes) = model.readerConfigPlan {
                        VStack(alignment:.leading,spacing:14) {
                            ForEach(changes) { change in
                                VStack(alignment:.leading,spacing:5) { Text(change.descriptions.joined(separator:"\n")).font(.callout); Text(change.detail).font(.caption).foregroundColor(.secondary) }
                            }
                        }.frame(maxWidth:.infinity,alignment:.leading)
                    }
                }.frame(maxHeight:360)
                HStack { Button("Cancel") { model.readerConfigPreview = false }; Spacer(); Button("Apply to reader") { model.applyReaderSettings() }.buttonStyle(.borderedProminent) }
            }.padding(28).frame(width:570)
        }
    }
    private func settingsGroup(_ group: String, _ snapshot: ReaderSettingsSnapshot) -> some View {
        VStack(alignment:.leading,spacing:12) {
            ForEach(ReaderSettingsCatalog.fields(snapshot.family).filter { $0.group == group }) { field in
                VStack(alignment:.leading,spacing:5) {
                    HStack {
                        Text(field.title).font(.callout)
                        Spacer()
                        Picker(field.title,selection:Binding<Int?>(get:{ model.readerEdits[field.id] ?? field.value(in:snapshot) },set:{ if let value = $0 { model.readerEdits[field.id] = value } else { model.readerEdits.removeValue(forKey:field.id) } })) {
                            if field.value(in:snapshot) == nil { Text("Choose…").tag(Int?.none) }
                            else if !field.choices.contains(where: { $0.value == field.value(in:snapshot) }) { Text(field.label(field.value(in:snapshot))).tag(field.value(in:snapshot)) }
                            ForEach(field.choices) { Text($0.label).tag(Optional($0.value)) }
                        }.labelsHidden().frame(width:220).disabled(model.busy || field.unavailable(in:snapshot) != nil)
                    }
                    if let reason = field.unavailable(in:snapshot) { Text(reason).font(.caption).foregroundColor(.secondary) }
                    else if !field.note.isEmpty { Text(field.note).font(.caption).foregroundColor(.secondary) }
                }
            }
        }
    }
}
