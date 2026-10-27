// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
import Darwin
import SwiftUI
import AppKit
import MagStripeCore
@testable import MagStripeStudio

final class M100SerialTests: XCTestCase {
    @MainActor func testFragmentedInventoryAndInterleavedNotifications() async throws {
        let fixture = try M100PTY()
        defer { fixture.close() }
        let transport = try M100SerialTransport(path: fixture.path, baud: 115200)
        defer { transport.close() }
        let report = try UHFProtocol.hex("C9340030751FEB705C5904E3D50D703A76")
        let notification = try M100Frame(kind: 2, command: 0x22, data: report).encoded()
        var tags: [UHFTag] = []
        transport.onTag = { tags.append($0) }
        let responder = fixture.respond([
            (try M100Frame(command: 0x22).encoded(), notification+notification),
            (try M100Frame(command: 0xB7).encoded(), notification + (try M100Frame(kind: 1, command: 0xB7, data: [7,0xD0]).encoded()))
        ])
        let scan = try await transport.exchange(0x22, data: [], timeout: 2)
        XCTAssertTrue(scan.isEmpty)
        let power = try await transport.exchange(0xB7, data: [], timeout: 2)
        XCTAssertEqual(power, [7,0xD0])
        try await responder.value
        XCTAssertEqual(tags.count, 3); XCTAssertEqual(tags.first?.signal, -55)
    }
    @MainActor func testNoTagIsAnEmptyScanAndTagErrorIsNotSuccess() async throws {
        let fixture = try M100PTY()
        defer { fixture.close() }
        let transport = try M100SerialTransport(path: fixture.path, baud: 9600)
        defer { transport.close() }
        let responder = fixture.respond([
            (try M100Frame(command: 0x22).encoded(), try M100Frame(kind: 1, command: 0xFF, data: [0x15]).encoded()),
            (try M100Frame(command: 0x39).encoded(), try M100Frame(kind: 1, command: 0xFF, data: [0x16]).encoded())
        ])
        let tags = try await transport.exchange(0x22, data: [], timeout: 2)
        XCTAssertTrue(tags.isEmpty)
        do { _ = try await transport.exchange(0x39, data: [], timeout: 2); XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("password")) }
        try await responder.value
    }
    @MainActor func testTimeoutAndCancellationCloseSerialConnection() async throws {
        for cancel in [false, true] {
            let fixture = try M100PTY()
            defer { fixture.close() }
            let transport = try M100SerialTransport(path: fixture.path, baud: 115200)
            defer { transport.close() }
            let task = Task { try await transport.exchange(0x03, data: [0], timeout: cancel ? 2 : 0.03) }
            if cancel { try await Task.sleep(nanoseconds: 20_000_000); task.cancel() }
            do { _ = try await task.value; XCTFail() } catch {}
            do { _ = try await transport.exchange(0x03, data: [0], timeout: 0.03); XCTFail() }
            catch { XCTAssertTrue(error.localizedDescription.contains("disconnected")) }
        }
    }
    @MainActor func testProfileDefaultsAndModuleLimitsInWorkspace() throws {
        let root = AppModel(scanDevices: false)
        root.selectedPage = .uhf
        let output = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks")
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        for profile in [UHFReaderProfile.jrd4035, .yrm100] {
            root.uhf.profile = profile; root.uhf.profileChanged()
            XCTAssertEqual(root.uhf.baud, 115200); XCTAssertEqual(root.uhf.profile.maxWriteBytes, 64)
            root.uhf.connected = true
            let epc = try UHFProtocol.hex("30751FEB705C5904E3D50D70")
            root.uhf.accept(UHFTag(epc: epc, pc: [0x34,0], signal: -55))
            root.uhf.writeData = String(repeating: "AA", count: 66)
            root.uhf.prepareWrite(epc: false)
            XCTAssertNil(root.uhf.writePreview)
            root.uhf.writeData = "AA BB"; root.uhf.word = "65536"
            root.uhf.prepareWrite(epc: false)
            XCTAssertNil(root.uhf.writePreview)
            root.uhf.setBuzzer(true)
            XCTAssertFalse(root.uhf.busy)
            let host = NSHostingView(rootView: ContentView().environmentObject(root))
            host.frame = NSRect(x: 0, y: 0, width: 1180, height: 1100)
            let window = NSWindow(contentRect: host.frame, styleMask: .borderless, backing: .buffered, defer: false)
            window.contentView = host; host.layoutSubtreeIfNeeded()
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                .write(to: output.appendingPathComponent(profile == .jrd4035 ? "uhf-jrd4035.png" : "uhf-yrm100.png"))
            window.contentView = nil; root.uhf.disconnect(silent: true)
        }
    }
}

@MainActor private final class M100PTY {
    private var master: Int32 = -1
    let path: String
    init() throws {
        var slave: Int32 = -1
        var name = [CChar](repeating: 0, count: 1024)
        guard openpty(&master, &slave, &name, nil, nil) == 0 else { throw XCTSkip("Pseudo terminal unavailable") }
        path = String(cString: name)
        _ = Darwin.close(slave); _ = fcntl(master, F_SETFL, O_NONBLOCK)
    }
    func close() { if master >= 0 { _ = Darwin.close(master); master = -1 } }
    func respond(_ commands: [([UInt8], [UInt8])]) -> Task<Void, Error> {
        Task { @MainActor in
            for (expected, response) in commands {
                var received: [UInt8] = []
                for _ in 0..<200 {
                    var buffer = [UInt8](repeating: 0, count: 1024)
                    let count = Darwin.read(self.master, &buffer, buffer.count)
                    if count > 0 { received += Array(buffer.prefix(count)) }
                    if received.count >= expected.count { break }
                    try await Task.sleep(nanoseconds: 5_000_000)
                }
                XCTAssertEqual(received, expected)
                for index in stride(from: 0, to: response.count, by: 3) {
                    let chunk = Array(response[index..<min(index+3, response.count)])
                    _ = chunk.withUnsafeBytes { Darwin.write(self.master, $0.baseAddress, chunk.count) }
                    try await Task.sleep(nanoseconds: 2_000_000)
                }
            }
        }
    }
}
