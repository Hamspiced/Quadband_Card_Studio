// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
import Darwin
import SwiftUI
import AppKit
import MagStripeCore
@testable import MagStripeStudio

final class ChameleonHardwareTests: XCTestCase {
    func testBluetoothSecurityReadPrecedesUARTSubscription() {
        var setup = ChameleonBLESetup()
        XCTAssertEqual(setup.uartDiscovered(hasBatteryService:true),.discoverBattery)
        XCTAssertEqual(setup.subscribed(),.none)
        XCTAssertEqual(setup.batteryDiscovered(readable:true),.readBattery)
        XCTAssertEqual(setup.subscribed(),.none)
        XCTAssertEqual(setup.batteryRead(),.subscribe)
        XCTAssertEqual(setup.batteryRead(),.none)
        XCTAssertEqual(setup.subscribed(),.ready)
        XCTAssertEqual(setup.subscribed(),.none)
    }
    func testBluetoothSetupFallbackAndCancelledCallbacks() {
        var withoutBattery = ChameleonBLESetup()
        XCTAssertEqual(withoutBattery.uartDiscovered(hasBatteryService:false),.subscribe)
        XCTAssertEqual(withoutBattery.subscribed(),.ready)
        var unreadable = ChameleonBLESetup()
        XCTAssertEqual(unreadable.uartDiscovered(hasBatteryService:true),.discoverBattery)
        XCTAssertEqual(unreadable.batteryDiscovered(readable:false),.subscribe)
        var cancelled = ChameleonBLESetup()
        _ = cancelled.uartDiscovered(hasBatteryService:true); cancelled.cancel()
        XCTAssertEqual(cancelled.batteryDiscovered(readable:true),.none)
        XCTAssertEqual(cancelled.batteryRead(),.none); XCTAssertEqual(cancelled.subscribed(),.none)
    }
    @MainActor func testConnectionHandlesBLEStyleFragmentsAndClosesOnTimeout() async throws {
        let channel = FakeLFChannel(); let transport = ChameleonConnection(channel:channel)
        channel.reply = try ChameleonFrame(command:1000,status:0x68,data:[2,1]).encoded()
        let reply = try await transport.exchange(1000,data:[],timeout:1)
        XCTAssertEqual(reply.data,[2,1]); XCTAssertEqual(channel.sent,try ChameleonFrame(command:1000).encoded())
        channel.reply = nil
        do { _ = try await transport.exchange(1000,data:[],timeout:0.02); XCTFail() } catch {}
        XCTAssertTrue(channel.closed)
        do { _ = try await transport.exchange(1000,data:[],timeout:1); XCTFail() } catch {}
    }
    @MainActor func testTransportRejectsWrongCommandAndCancellation() async throws {
        let channel = FakeLFChannel(); let transport = ChameleonConnection(channel:channel)
        channel.reply = try ChameleonFrame(command:1035,status:0x68).encoded()
        do { _ = try await transport.exchange(1000,data:[],timeout:1); XCTFail() } catch {}
        XCTAssertTrue(channel.closed)
        let other = FakeLFChannel(); let connection = ChameleonConnection(channel:other)
        let task = Task { try await connection.exchange(1000,data:[],timeout:2) }
        try await Task.sleep(nanoseconds:20_000_000); task.cancel()
        do { _ = try await task.value; XCTFail() } catch { XCTAssertTrue(error is CancellationError) }
        XCTAssertTrue(other.closed)
    }
    @MainActor func testUSBTransportWithPseudoTerminal() async throws {
        var master: Int32 = -1, slave: Int32 = -1; var name = [CChar](repeating:0,count:1024)
        guard openpty(&master,&slave,&name,nil,nil) == 0 else { throw XCTSkip("Pseudo terminal unavailable") }
        defer { _ = Darwin.close(master) }; _ = Darwin.close(slave); _ = fcntl(master,F_SETFL,O_NONBLOCK)
        let transport = try ChameleonConnection(path:String(cString:name)); defer { transport.close() }
        let responder = Task { @MainActor in
            var buffer = [UInt8](repeating:0,count:1024)
            for _ in 0..<100 {
                let count = Darwin.read(master,&buffer,buffer.count)
                if count > 0 {
                    XCTAssertEqual(Array(buffer.prefix(count)),try ChameleonFrame(command:1000).encoded())
                    let reply = try ChameleonFrame(command:1000,status:0x68,data:[2,1]).encoded()
                    for chunk in [Array(reply.prefix(5)),Array(reply.dropFirst(5))] {
                        _ = chunk.withUnsafeBytes { Darwin.write(master,$0.baseAddress,chunk.count) }; try await Task.sleep(nanoseconds:10_000_000)
                    }
                    return
                }
                try await Task.sleep(nanoseconds:10_000_000)
            }
            throw StripeError("No command received")
        }
        let result = try await transport.exchange(1000,data:[],timeout:2); XCTAssertEqual(result.data,[2,1]); try await responder.value
    }
    @MainActor func testRenderLFWorkspaceAndPreview() throws {
        _ = NSApplication.shared
        let root = AppModel(scanDevices:false); root.selectedPage = .lf
        let model = root.lf
        model.ports = [SerialDevice(path:"/dev/cu.usbmodem0000000000001",name:"Chameleon Ultra",vendor:0x6868,product:0x8686)]
        model.selectedPort = model.ports[0].path
        model.card = try LFCard(format:.em410x,bytes:[1,2,3,4,5])
        let host = NSHostingView(rootView:ContentView().environmentObject(root)); host.frame = NSRect(x:0,y:0,width:1180,height:1080)
        let window = NSWindow(contentRect:host.frame,styleMask:.borderless,backing:.buffered,defer:false); window.contentView = host
        host.layoutSubtreeIfNeeded(); RunLoop.current.run(until:Date().addingTimeInterval(0.08)); host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in:host.bounds)); host.cacheDisplay(in:host.bounds,to:bitmap)
        let path = URL(fileURLWithPath:FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks/lf-workspace.png")
        try FileManager.default.createDirectory(at:path.deletingLastPathComponent(),withIntermediateDirectories:true)
        try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).write(to:path); window.contentView = nil
        model.prepare(.defaults); XCTAssertNil(model.preview)
        model.clear(); XCTAssertNil(model.card); XCTAssertEqual(model.defaultID,"0102030405")
    }
    @MainActor func testHFWorkflowLocksLFControlsAndRejectsDisconnectedRouting() async throws {
        let root = AppModel(scanDevices:false); root.lf.connected = true
        XCTAssertTrue(root.lf.canOperate)
        root.nfc.busy = true; XCTAssertFalse(root.lf.canOperate)
        root.lf.disconnect(); XCTAssertTrue(root.lf.connected)
        root.nfc.busy = false; XCTAssertTrue(root.lf.canOperate)
        root.lf.connected = false
        do { _ = try await root.nfc.service.identify(reader:"Chameleon Ultra · USB"); XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("Reconnect")) }
    }
    @MainActor func testLiveChameleonUSBReadOnly() async throws {
        guard let path = ProcessInfo.processInfo.environment["QUADBAND_LF_TEST_PORT"] else { throw XCTSkip("Opt-in Chameleon USB check") }
        let transport = try ChameleonConnection(path:path); defer { transport.close() }
        let client = ChameleonClient(transport:transport)
        let info = try await client.identify(); print("LIVE CHAMELEON: \(info.version); formats \(info.formats.map(\.rawValue))")
        do {
            let card = try await client.scan(); print("LIVE LF: \(card?.format.rawValue ?? "No supported LF tag") \(card?.identifier ?? "")")
            try await client.restoreMode()
        } catch let failure { print("LIVE READ ERROR: \(failure.localizedDescription)"); try? await client.restoreMode(); throw failure }
    }
    @MainActor func testLiveChameleonHFUSBReadOnly() async throws {
        guard let path = ProcessInfo.processInfo.environment["QUADBAND_HF_CHAMELEON_PORT"] else { throw XCTSkip("Opt-in Chameleon HF check") }
        let transport = try ChameleonConnection(path:path); defer { transport.close() }
        transport.onDisconnect = { print("LIVE HF DISCONNECT: \($0.localizedDescription)") }
        let client = ChameleonClient(transport:transport); let info = try await client.identify(); print("LIVE HF CONNECTED: \(info.version)")
        do {
            let identified = try await ChameleonHFClient(client:client).identify(); print("LIVE HF IDENTIFIED: \(identified)")
            let snapshot = try await ChameleonHFClient(client:client).read(key:Array(repeating:255,count:6),type:.a)
            print("LIVE CHAMELEON HF: \(snapshot.card.name) · \(HexBytes.string(snapshot.card.uid)); blocks \(snapshot.blocks.count), errors \(snapshot.errors.count)")
            if let ndef = try? ClassicNDEF(snapshot:snapshot) { print("LIVE HF RECORDS: \(ndef.records.map(\.displayValue))") }
            XCTAssertEqual(snapshot.blocks.count,64); XCTAssertTrue(snapshot.errors.isEmpty)
            try await client.restoreMode()
        } catch let failure { print("LIVE READ ERROR: \(failure.localizedDescription)"); try? await client.restoreMode(); throw failure }
    }
    @MainActor func testLiveChameleonBluetoothReadOnly() async throws {
        guard ProcessInfo.processInfo.environment["QUADBAND_LF_BLE_TEST"] == "1" else { throw XCTSkip("Opt-in Bluetooth check") }
        let bluetooth = ChameleonBluetooth(); defer { bluetooth.close() }; bluetooth.scan()
        for _ in 0..<120 {
            if !bluetooth.devices.isEmpty { break }; try await Task.sleep(nanoseconds:100_000_000)
        }
        let device = try XCTUnwrap(bluetooth.devices.first(where: { $0.name.localizedCaseInsensitiveContains("chameleon") }),bluetooth.message)
        let transport = ChameleonConnection(channel:try await bluetooth.connect(device.id)); defer { transport.close() }
        let client = ChameleonClient(transport:transport); let info = try await client.identify()
        print("LIVE BLUETOOTH: \(device.name) · \(info.version)")
        do {
            let card = try await client.scan(); print("LIVE BLE LF: \(card?.identifier ?? "No supported tag")")
            let hf = ChameleonHFClient(client:client)
            let snapshot = try await hf.read(key:Array(repeating:255,count:6),type:.a)
            print("LIVE BLE HF: \(HexBytes.string(snapshot.card.uid)); blocks \(snapshot.blocks.count), errors \(snapshot.errors.count)")
            if let ndef = try? ClassicNDEF(snapshot:snapshot) { print("LIVE BLE RECORDS: \(ndef.records.map(\.displayValue))") }
            XCTAssertEqual(snapshot.blocks.count,64)
            try await client.restoreMode()
        }
        catch { try? await client.restoreMode(); throw error }
    }
}
@MainActor private final class FakeLFChannel: ChameleonByteChannel {
    var onBytes: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    var sent: [UInt8] = [], reply: [UInt8]?
    var closed = false
    func close() { closed = true }
    func send(_ bytes: [UInt8]) throws {
        sent += bytes
        if let reply { Task { @MainActor in for byte in reply { onBytes?([byte]) } } }
    }
}
