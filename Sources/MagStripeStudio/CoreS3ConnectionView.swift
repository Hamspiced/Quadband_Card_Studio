import SwiftUI
import MagStripeCore

struct CoreS3ConnectionView: View {
    @ObservedObject var hub: CoreS3Hub
    var body: some View {
        VStack(alignment:.leading,spacing:10) {
            Text("M5STACK CORES3").font(.system(size:10,weight:.semibold)).tracking(1.5).foregroundColor(.white.opacity(0.5))
            Label(hub.capabilities == nil ? "Not connected" : "Paired reader hub",systemImage:hub.capabilities == nil ? "antenna.radiowaves.left.and.right" : "checkmark.shield.fill")
                .font(.system(size:12)).foregroundColor(hub.capabilities == nil ? .white : .mint)
            if hub.capabilities == nil {
                Picker("CoreS3 device",selection:$hub.selected) {
                    Text("Choose CoreS3").tag(nil as UUID?)
                    ForEach(hub.devices) { Text($0.name).tag(Optional($0.id)) }
                }.labelsHidden().disabled(hub.connecting)
                HStack {
                    Button(hub.scanning ? "Stop" : "Scan") { if hub.scanning { hub.stopScan() } else { hub.scan() } }.disabled(hub.connecting)
                    Button("Pair / connect") { hub.connect() }.disabled(hub.selected == nil || hub.connecting)
                    if hub.connecting { ProgressView().controlSize(.small) }
                }.controlSize(.small)
                if hub.connecting { Button("Cancel connection") { hub.cancelConnection() }.controlSize(.small) }
            } else {
                if let value = hub.capabilities {
                    Text("\(value.name) · \(value.firmware)").font(.system(size:10))
                    Text(value.modeDescription).font(.system(size:10,weight:.medium))
                        .foregroundColor(value.remoteAllowed ? .mint : .orange)
                        .fixedSize(horizontal:false,vertical:true)
                    ForEach(value.peripherals,id:\.channel) { slot in
                        HStack(alignment:.top,spacing:6) {
                            Image(systemName:slot.present ? "checkmark.circle" : "minus.circle")
                            Text("\(slot.channel == 1 ? "Port A HF" : slot.channel == 2 ? "Port C UHF" : "USB Chameleon") · \(slot.present ? slot.kind : "absent")")
                        }.font(.system(size:10)).foregroundColor(slot.present ? .mint : .white.opacity(0.5))
                    }
                }
                Button("Disconnect hub") { hub.disconnect() }.controlSize(.small).disabled(hub.connecting)
                Button("Refresh modules") { hub.refreshPeripherals() }.controlSize(.small).disabled(hub.connecting)
            }
            Text(hub.message).font(.system(size:10)).foregroundColor(.white.opacity(0.65)).fixedSize(horizontal:false,vertical:true)
            Text("One BLE session. Select Grove HF in HF readers, CoreS3 in UHF connection, or CoreS3 under Chameleon. Pairing requires local screen approval.").font(.system(size:10)).foregroundColor(.white.opacity(0.5)).fixedSize(horizontal:false,vertical:true)
        }.accessibilityIdentifier("cores3Connection")
    }
}

struct CoreS3AuthenticationView: View {
    @ObservedObject var hub: CoreS3Hub
    var onChange: () -> Void = {}
    var body: some View {
        VStack(alignment:.leading,spacing:10) {
            Toggle("Use supplied NTAG password for this session",isOn:$hub.usePassword).disabled(!hub.supportsAuthentication)
            if hub.usePassword {
                HStack {
                    SecureField("Password · 8 hex digits",text:$hub.password)
                    SecureField("Expected PACK · 4 hex digits (optional)",text:$hub.pack)
                    Button("Clear credential") { hub.password = ""; hub.pack = ""; hub.usePassword = false }
                }.textFieldStyle(.roundedBorder)
            }
            Text(hub.supportsAuthentication ? "Credentials are transient and never saved in card profiles. Password/PACK and protection settings are never changed." : "The detected HF module does not advertise password authentication. Protected pages remain read-only.")
                .font(.caption).foregroundColor(.secondary)
        }.onChange(of:hub.usePassword) { _ in onChange() }
            .onChange(of:hub.password) { _ in onChange() }
            .onChange(of:hub.pack) { _ in onChange() }
    }
}
