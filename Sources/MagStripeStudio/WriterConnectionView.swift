// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit

struct BrandLogo: View {
    private var logo: NSImage? {
        let url = Bundle.main.url(forResource: "MidwestGadgets", withExtension: "png")
            ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("Resources/MidwestGadgets.png")
        return NSImage(contentsOf: url)
    }
    var body: some View {
        if let logo { Image(nsImage: logo).resizable().interpolation(.high).antialiased(true).scaledToFit().accessibilityLabel("Midwest Gadgets logo") }
    }
}

struct WriterConnectionView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var bluetooth: BluetoothController
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("MAGSTRIPE WRITER").font(.system(size: 10, weight: .semibold)).tracking(1.5).foregroundColor(.white.opacity(0.5))
            Picker("Connection", selection: $model.connectionMode) {
                ForEach(AppModel.ConnectionMode.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }.pickerStyle(.segmented).labelsHidden().disabled(model.busy || model.connected)
            Label(model.connected ? "Connected" : "Not connected", systemImage: model.connected ? "checkmark.circle.fill" : "antenna.radiowaves.left.and.right")
                .font(.system(size: 12, weight: .medium)).foregroundColor(model.connected ? .mint : .white.opacity(0.85))
            if model.connectionMode == .usb {
                if model.devices.isEmpty {
                    Text("Connect your MSR X6 with a USB data cable.").font(.system(size: 11)).foregroundColor(.white.opacity(0.55))
                } else {
                    Picker("Writer", selection: $model.selectedDevice) {
                        ForEach(model.devices) { Text($0.name).tag($0.id) }
                    }.labelsHidden().disabled(model.busy || model.connected)
                }
            } else if model.connectionMode == .serial {
                Picker("MSR605 port",selection:$model.selectedSerial) {
                    Text("Choose MSR605 port").tag("")
                    ForEach(model.serialPorts) { Text($0.label).tag($0.path) }
                }.labelsHidden().disabled(model.busy || model.connected)
                Picker("Baud",selection:$model.serialBaud) {
                    ForEach([9600,19200,38400,57600,115200],id:\.self) { Text(String($0)).tag($0) }
                }.disabled(model.busy || model.connected)
                Text("MSR605: USB serial, usually 9600 baud. MSR605X uses USB HID. Select the magstripe device's port.").font(.system(size:11)).foregroundColor(.white.opacity(0.55)).fixedSize(horizontal:false,vertical:true)
            } else {
                if !bluetooth.devices.isEmpty {
                    Picker("Bluetooth writer", selection: $model.selectedBluetooth) {
                        Text("Choose reader").tag(UUID?.none)
                        ForEach(bluetooth.devices) { Text($0.name).tag(Optional($0.id)) }
                    }.labelsHidden().disabled(model.busy || model.connected)
                }
                Text(bluetooth.message).font(.system(size: 11)).foregroundColor(.white.opacity(0.55)).fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Button(model.connected ? "Disconnect" : "Connect") {
                    if model.connected { model.disconnect() } else { model.connect() }
                }.disabled(model.busy || (!model.connected && (model.connectionMode == .usb ? model.devices.isEmpty : model.connectionMode == .serial ? model.selectedSerial.isEmpty : bluetooth.devices.isEmpty)))
                if model.connectionMode != .bluetooth {
                    Button { model.refresh() } label: { Image(systemName: "arrow.clockwise") }.help("Refresh USB devices")
                        .disabled(model.busy || model.connected)
                } else {
                    Button(bluetooth.scanning ? "Stop" : "Scan") { if bluetooth.scanning { bluetooth.stopScan() } else { bluetooth.scan() } }
                        .disabled(model.busy || model.connected)
                }
            }.controlSize(.small)
        }
        .onChange(of: bluetooth.devices.count) { _ in
            if model.selectedBluetooth == nil { model.selectedBluetooth = bluetooth.devices.first?.id }
        }
    }
}
