// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
import Darwin
import SwiftUI
import AppKit
import MagStripeCore
@testable import MagStripeStudio

final class ReaderHardwareTests: XCTestCase {
    @MainActor func testSerialMSR605HandshakeOverPseudoTerminal() async throws {
        let pty = try TestPTY(); defer { pty.close() }
        let channel = try SerialMSRTransport(path:pty.path,baud:9600); defer { channel.close() }
        let responder = Task { @MainActor in
            var commands: [UInt8] = []
            for _ in 0..<150 {
                commands += pty.read()
                if commands.suffix(2) == [0x1B,0x65] {
                    pty.send([0x1B]); try await Task.sleep(nanoseconds:20_000_000); pty.send([0x79]); return commands
                }
                try await Task.sleep(nanoseconds:10_000_000)
            }
            throw StripeError("Fake writer never received the handshake")
        }
        try await MSRClient(transport:channel).handshake()
        let sent = try await responder.value
        XCTAssertEqual(sent,MSRProtocol.reset+MSRProtocol.ping)
    }
    @MainActor func testSerialTimeoutAndCancellationResumeWaitingCaller() async throws {
        let pty = try TestPTY(); defer { pty.close() }
        let channel = try SerialMSRTransport(path:pty.path,baud:9600); defer { channel.close() }
        do { _ = try await channel.exchange(MSRProtocol.ping,timeout:0.05); XCTFail() } catch { XCTAssertTrue(error.localizedDescription.contains("timed out")) }
        let task = Task { try await channel.exchange(MSRProtocol.read,timeout:3) }
        try await Task.sleep(nanoseconds:30_000_000); task.cancel()
        do { _ = try await task.value; XCTFail() } catch { XCTAssertTrue(error is CancellationError) }
    }
    @MainActor func testPiswordsTransportFragmentedPacketsAndInterleavedInventory() async throws {
        let pty = try TestPTY(); defer { pty.close() }
        let channel = try PiswordsTransport(path:pty.path,baud:9600,address:0); defer { channel.close() }
        var tags: [UHFTag] = []; channel.onTag = { tags.append($0) }
        let responder = Task { @MainActor in
            for _ in 0..<100 {
                if !pty.read().isEmpty {
                    let tag = try UHFProtocol.hex("A0 19 00 89 01 30 00 E2 80 68 94 00 00 40 30 C6 12 8C CC E6 44 C7 F0 0D C4 6A 73")
                    let ack = try UHFFrame(address:0,command:0x89,data:[0x10]).encoded()
                    pty.send(Array(tag.prefix(8))); try await Task.sleep(nanoseconds:20_000_000); pty.send(Array(tag.dropFirst(8))+ack); return
                }
                try await Task.sleep(nanoseconds:10_000_000)
            }
            throw StripeError("No command received")
        }
        let reply = try await channel.exchange(0x89,data:[1],timeout:2)
        XCTAssertEqual(reply,[0x10]); XCTAssertEqual(tags.count,1)
        try await responder.value
    }
    @MainActor func testUHFWritePreviewAndSessionState() throws {
        let model = UHFModel(); model.autoDiscovery = false
        model.connected = true
        let tag = UHFTag(epc:[1,2,3,4],pc:[0x10,0]); model.accept(tag)
        model.newEPC = "AB CD 12 34"; model.prepareWrite(epc:true)
        XCTAssertEqual(model.writePreview?.oldEPC,tag.epc); XCTAssertEqual(model.writePreview?.bytes,[0xAB,0xCD,0x12,0x34])
        model.writePreview = nil; model.scanning = true; model.prepareWrite(epc:true); XCTAssertNil(model.writePreview)
        model.scanning = false; model.password = "BAD"; model.prepareWrite(epc:true); XCTAssertNil(model.writePreview)
        model.clear(); XCTAssertTrue(model.tags.isEmpty); XCTAssertEqual(model.password,"00000000")
    }
    @MainActor func testRenderUHFAndReaderTools() throws {
        _ = NSApplication.shared
        let model = AppModel(scanDevices:false)
        model.selectedPage = .uhf
        model.uhf.ports = [SerialDevice(path:"/dev/cu.usbmodem21101",name:"USB Serial",vendor:0x1A86,product:0xFE0C)]
        model.uhf.selectedPort = model.uhf.ports[0].path; model.uhf.baud = 9600; model.uhf.connected = true
        model.uhf.version = "UCM601 · firmware 2.3"; model.uhf.details = "Antenna 1 · 20 dBm · Region 1 · channels 7–59"
        model.uhf.accept(UHFTag(epc:try UHFProtocol.hex("E2 80 68 94 00 00 40 30 C6 12 8C CC"),pc:[0x30,0],frequencyKHz:902250,signal:-57))
        model.uhf.note("Scan stopped. 1 unique EPC found.",.success)
        try render(ContentView().environmentObject(model),name:"uhf-workspace",width:1180,height:1000)
        model.connectionMode = .serial; model.selectedPage = .write
        try render(ContentView().environmentObject(model),name:"msr605-connection",width:1180,height:860)
        for family in ReaderFamily.allCases {
            let name = family == .acr1552 ? "ACS ACR1552 1S CL Reader" : "ACS ACR122U PICC Interface"
            model.selectedPage = .nfc; model.nfc.hfPage = .tools
            model.nfc.readers = [name]; model.nfc.selectedReader = name
            var snapshot = ReaderSettingsSnapshot(reader:name,family:family,firmware:family == .acr1552 ? "ACR1552 R FW 1.06.00" : "ACR122U201")
            snapshot.values = [.ui:[0x6F],.polling:[0x8B],.operating:[0xFF],.power:[0],.types:[7,5],.pps:[0,0],.host:[1],.hid:[0,0],.suspend:[0],.characters:[255,0x28,255],.keyboard:[0]]
            model.nfc.readerSettings = snapshot; model.nfc.readerEdits = [:]; model.nfc.stageReaderPreset("mute")
            try render(ContentView().environmentObject(model),name:"tools-"+family.rawValue,width:1180,height:1000)
        }
    }
    @MainActor private func render<V: View>(_ view: V,name: String,width: CGFloat,height: CGFloat) throws {
        let host = NSHostingView(rootView:view); host.frame = NSRect(x:0,y:0,width:width,height:height)
        let window = NSWindow(contentRect:host.frame,styleMask:.borderless,backing:.buffered,defer:false)
        window.contentView = host; host.layoutSubtreeIfNeeded(); RunLoop.current.run(until:Date().addingTimeInterval(0.08)); host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in:host.bounds)); host.cacheDisplay(in:host.bounds,to:bitmap)
        let data = try XCTUnwrap(bitmap.representation(using:.png,properties:[:])); XCTAssertGreaterThan(data.count,5000)
        let url = URL(fileURLWithPath:FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks/\(name).png")
        try FileManager.default.createDirectory(at:url.deletingLastPathComponent(),withIntermediateDirectories:true); try data.write(to:url); window.contentView = nil
    }
    @MainActor func testLivePiswordsReadOnly() async throws {
        guard let path = ProcessInfo.processInfo.environment["QUADBAND_UHF_TEST_PORT"] else { throw XCTSkip("Opt-in connected Piswords check") }
        let channel = try PiswordsTransport(path:path,baud:9600,address:0); defer { channel.close() }
        let client = UHFClient(transport:channel)
        let version = try UHFProtocol.version(await client.command(0x72)); print("LIVE UHF: \(version)")
        var tags: [UHFTag] = []; channel.onTag = { tag in if !tags.contains(where: { $0.id == tag.id }) { tags.append(tag) } }
        try UHFProtocol.acknowledge(await client.command(0x89,[1]))
        try await Task.sleep(nanoseconds:600_000_000)
        try UHFProtocol.acknowledge(await client.command(0x8C))
        let tag = try XCTUnwrap(tags.first); print("LIVE UHF EPC: \(tag.id)")
        let tid = try await client.read(epc:tag.epc,bank:.tid,word:0,count:6,password:[0,0,0,0])
        print("LIVE UHF TID: \(HexBytes.string(tid.bytes))")
        let epc = try await client.read(epc:tag.epc,bank:.epc,word:1,count:tag.epc.count/2+1,password:[0,0,0,0])
        XCTAssertEqual(epc.bytes,tag.pc+tag.epc)
        try UHFProtocol.acknowledge(await client.command(0x85,[1]))
    }
    func testLiveACR122ReadSettingsOnly() async throws {
        guard ProcessInfo.processInfo.environment["QUADBAND_ACR_TEST"] == "1" else { throw XCTSkip("Opt-in connected ACR122 check") }
        let readers = try await NFCService().readers()
        let name = try XCTUnwrap(readers.first(where: { ReaderFamily.detect($0) == .acr122 }))
        let snapshot = try await ReaderConfigService().read(reader:name)
        print("LIVE ACS: \(snapshot.firmware), settings \(snapshot.values), errors \(snapshot.errors)")
        XCTAssertEqual(snapshot.family,.acr122); XCTAssertNotNil(snapshot.values[.operating])
    }
}
@MainActor private final class TestPTY {
    var master: Int32 = -1
    let path: String
    init() throws {
        var master: Int32 = -1, slave: Int32 = -1
        var name = [CChar](repeating:0,count:1024)
        guard openpty(&master,&slave,&name,nil,nil) == 0 else { throw XCTSkip("Pseudo terminal unavailable: \(String(cString:strerror(errno)))") }
        self.master = master; self.path = String(cString:name)
        _ = Darwin.close(slave); _ = fcntl(master,F_SETFL,O_NONBLOCK)
    }
    func read() -> [UInt8] { var bytes = [UInt8](repeating:0,count:4096); let count = Darwin.read(master,&bytes,bytes.count); return count > 0 ? Array(bytes.prefix(count)) : [] }
    func send(_ bytes: [UInt8]) { _ = bytes.withUnsafeBytes { Darwin.write(master,$0.baseAddress,bytes.count) } }
    func close() { if master >= 0 { _ = Darwin.close(master); master = -1 } }
}
