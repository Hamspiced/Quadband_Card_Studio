// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI

struct ChameleonConnectionView: View {
    @ObservedObject var model: LFModel
    @ObservedObject var bluetooth: ChameleonBluetooth
    private var locked: Bool { model.busy || model.hfWorkflowBusy || model.connected }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("CHAMELEON ULTRA").font(.system(size: 10, weight: .semibold)).tracking(1.5).foregroundColor(.white.opacity(0.5))
            Picker("Chameleon connection", selection: $model.link) {
                ForEach(LFModel.Link.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }.pickerStyle(.segmented).labelsHidden().disabled(locked)
            Label(model.connected ? "Connected · HF / LF" : "Not connected",
                  systemImage: model.connected ? "checkmark.circle.fill" : "wave.3.right")
                .font(.system(size: 12, weight: .medium)).foregroundColor(model.connected ? .mint : .white.opacity(0.85))
            if model.link == .usb {
                Picker("Chameleon USB port", selection: $model.selectedPort) {
                    Text("Choose Chameleon port").tag("")
                    ForEach(model.ports) { Text($0.label).tag($0.path) }
                }.labelsHidden().disabled(locked)
            } else {
                Picker("Chameleon Bluetooth device", selection: $model.selectedBluetooth) {
                    Text("Choose Chameleon").tag(nil as UUID?)
                    ForEach(bluetooth.devices) { Text($0.name).tag(Optional($0.id)) }
                }.labelsHidden().disabled(locked)
                Text(bluetooth.message).font(.system(size: 11)).foregroundColor(.white.opacity(0.55))
                    .fixedSize(horizontal: false, vertical: true)
                Text("Pair with your configured Bluetooth PIN when macOS prompts.")
                    .font(.system(size: 10)).foregroundColor(.white.opacity(0.55)).fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Button(model.connected ? "Disconnect" : "Connect") {
                    if model.connected { model.disconnect() } else { model.connect() }
                }.disabled(model.busy || model.hfWorkflowBusy || (!model.connected && (model.link == .usb ? model.selectedPort.isEmpty : model.selectedBluetooth == nil)))
                if model.link == .usb {
                    Button { model.refresh() } label: { Image(systemName: "arrow.clockwise") }
                        .help("Refresh Chameleon USB devices").disabled(locked)
                } else {
                    Button(bluetooth.scanning ? "Stop" : "Scan") {
                        if bluetooth.scanning { bluetooth.stop() } else { bluetooth.scan() }
                    }.disabled(locked)
                }
            }.controlSize(.small)
            if let info = model.info {
                Text(info.version).font(.system(size: 10)).foregroundColor(.white.opacity(0.55))
            }
            HStack(alignment: .top, spacing: 6) {
                if model.busy { ProgressView().controlSize(.small) }
                Text(model.status).font(.system(size: 11))
                    .foregroundColor(model.statusKind == .error ? .orange : .white.opacity(0.65))
                    .fixedSize(horizontal: false, vertical: true)
            }
            if model.busy && !model.hfWorkflowBusy {
                Button("Cancel connection / operation") { model.cancel() }.controlSize(.small)
            }
        }
        .accessibilityIdentifier("chameleonConnection")
        .onChange(of: model.link) { _ in bluetooth.stop() }
        .onAppear { if model.autoDiscovery && model.ports.isEmpty { model.refresh() } }
    }
}
