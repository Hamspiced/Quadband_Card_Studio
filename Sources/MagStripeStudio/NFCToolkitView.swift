// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI

struct NFCToolkitView: View {
    @ObservedObject var model: NFCModel
    private let accent = Color(red: 0, green: 0.43, blue: 0.40)
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Label("NFC TOOLKIT", systemImage: "square.stack.3d.up").font(.system(size: 11, weight: .semibold))
                Spacer()
                Button("Save credentials") { model.saveCredentials() }.disabled(model.busy || !model.canSaveCredentials)
                Button("Write from file") { model.chooseCredentials() }.disabled(model.busy)
                Button("Copy to infinity") { model.batchPresented.toggle() }
                    .disabled(model.busy).tint(accent)
            }
            if model.batchPresented {
                VStack(alignment: .leading, spacing: 12) {
                    Label("Copy to infinity", systemImage: "infinity").font(.system(size: 20, weight: .semibold))
                    Text("Scan card or upload card credentials").font(.system(size: 13, weight: .medium))
                    HStack {
                        Button("Scan card") { model.read(forProfile: true) }.disabled(model.busy || model.selectedReader.isEmpty)
                        Button("Upload card credentials") { model.chooseCredentials() }.disabled(model.busy)
                        Spacer()
                        if model.batchRunning {
                            Button("Stop batch") { model.stopBatch() }.buttonStyle(.borderedProminent)
                        } else {
                            Button("Start copying") { model.startBatch() }.buttonStyle(.borderedProminent)
                                .disabled(model.busy || model.profile == nil || model.selectedReader.isEmpty)
                        }
                    }
                    Text("Remove the source card, then present targets one at a time. Each copy verifies all 47 data blocks. Failures are recorded and the batch continues after you remove that card.")
                        .font(.system(size: 11)).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                }.padding(16).background(accent.opacity(0.05)).cornerRadius(9)
            }
            if let profile = model.profile {
                HStack(alignment: .center) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(model.profileOrigin).font(.system(size: 13, weight: .semibold))
                        Text("\(profile.blocks.count) data blocks · 752 bytes · UID and keys preserved")
                            .font(.system(size: 11)).foregroundColor(.secondary)
                    }
                    Spacer()
                    if !model.batchPresented {
                        Button("Write profile & verify") { model.writeProfileOnce() }.buttonStyle(.borderedProminent)
                            .disabled(model.busy || model.selectedReader.isEmpty)
                    }
                }
            } else if !model.batchPresented {
                Text("Read all sectors to save a data-only .qcard file. Load it to write one card or a continuous batch.")
                    .font(.system(size: 11)).foregroundColor(.secondary)
            }
            if !model.copyResults.isEmpty {
                HStack {
                    Label("\(model.copiesSucceeded) succeeded", systemImage: "checkmark.circle.fill").foregroundColor(accent)
                    Label("\(model.copiesFailed) failed", systemImage: "xmark.circle.fill").foregroundColor(.orange)
                    Spacer()
                    Text("Latest 500 cards").foregroundColor(.secondary)
                }.font(.system(size: 11, weight: .medium))
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        ForEach(model.copyResults) { result in
                            HStack(alignment: .top, spacing: 10) {
                                Image(systemName: result.success ? "checkmark.circle.fill" : "xmark.circle.fill")
                                    .foregroundColor(result.success ? accent : .orange)
                                VStack(alignment: .leading, spacing: 3) {
                                    Text("Card \(result.number) · \(result.success ? "Success" : "Failed") · \(result.time.formatted(date: .omitted, time: .standard))")
                                        .font(.system(size: 11, weight: .semibold))
                                    Text(result.detail).font(.system(size: 11)).foregroundColor(.secondary).textSelection(.enabled)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                            }.frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }.padding(.trailing, 8)
                }.frame(height: min(CGFloat(model.copyResults.count) * 62, 240))
            }
        }.padding(20).background(.white).cornerRadius(12)
    }
}
