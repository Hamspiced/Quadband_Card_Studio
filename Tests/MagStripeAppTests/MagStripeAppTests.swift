// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
import AppKit
import SwiftUI
import MagStripeCore
@testable import MagStripeStudio

final class MagStripeAppTests: XCTestCase {
    @MainActor func testKeyboardReadAndUseInEditor() {
        let model = AppModel(scanDevices: false)
        model.selectedPage = .read
        model.armKeyboard()
        model.acceptKeyboard("%MIDWEST TEST?;123=456?+789=012?\r")
        XCTAssertEqual(model.readCard?.tracks, ["MIDWEST TEST", "123=456", "789=012"])
        XCTAssertFalse(model.keyboardArmed)
        XCTAssertEqual(model.statusKind, .success)
        model.useReadData()
        XCTAssertEqual(model.drafts, ["MIDWEST TEST", "123=456", "789=012"])
        XCTAssertEqual(model.selectedPage, .write)
        model.clearSession()
        XCTAssertNil(model.readCard)
        XCTAssertEqual(model.drafts, ["", "", ""])
        XCTAssertTrue(model.log.isEmpty)
    }

    @MainActor func testUnmarkedSwipeCanBeReassignedWithoutAnotherSwipe() {
        let model = AppModel(scanDevices: false)
        model.armKeyboard(); model.acceptKeyboard("4111111111111111=2912\r")
        XCTAssertNil(model.readCard)
        XCTAssertFalse(model.rawSwipe.isEmpty)
        model.inputFormat = .track2; model.reparseSwipe()
        XCTAssertEqual(model.readCard?.tracks, ["", "4111111111111111=2912", ""])
        XCTAssertEqual(model.statusKind, .success)
    }

    @MainActor func testNFCWriteRequiresReadAndValidInput() {
        let model = NFCModel()
        model.draft = "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F"
        XCTAssertNil(model.draftError)
        XCTAssertFalse(model.canWrite)
        model.write()
        XCTAssertFalse(model.busy)
        model.key = "BAD"
        XCTAssertNotNil(model.draftError)
        model.clear()
        XCTAssertEqual(model.key, "FF FF FF FF FF FF")
        XCTAssertEqual(model.draft, "")
    }

    @MainActor func testIncompleteSwipeDoesNotLeaveOldResults() {
        let model = AppModel(scanDevices: false)
        model.armKeyboard(); model.acceptKeyboard(";123?")
        XCTAssertNotNil(model.readCard)
        model.armKeyboard(); model.acceptKeyboard(";INCOMPLETE")
        XCTAssertNil(model.readCard)
        XCTAssertEqual(model.statusKind, .error)
        XCTAssertFalse(model.keyboardArmed)
    }

    @MainActor func testUSBCommandsStayDisabledWithoutConnection() {
        let model = AppModel(scanDevices: false)
        model.write(); model.readUSB()
        XCTAssertFalse(model.busy)
        XCTAssertFalse(model.connected)
        model.connect()
        XCTAssertEqual(model.statusKind, .error)
        XCTAssertFalse(model.busy)
    }

    @MainActor func testRealCaptureViewReceivesKeyboardEvents() async throws {
        _ = NSApplication.shared
        let view = SwipeCaptureView(frame: NSRect(x: 0, y: 0, width: 10, height: 10))
        let done = expectation(description: "Swipe completion")
        var received = ""
        view.onSwipe = { text in received = text; done.fulfill() }
        view.onError = { message in XCTFail(message) }
        view.setArmed(true, generation: 1)
        let text = "%MIDWEST?;123?+456?\r"
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                                    windowNumber: 0, context: nil, characters: text,
                                    charactersIgnoringModifiers: text, isARepeat: false, keyCode: 0)!
        view.keyDown(with: event)
        await fulfillment(of: [done], timeout: 3)
        XCTAssertEqual(received, text)
        XCTAssertEqual(try KeyboardSwipeParser.parse(received), CardData(["MIDWEST", "123", "456"]))
    }

    @MainActor func testFocusLossCancelsPendingCapture() async {
        let view = SwipeCaptureView()
        let done = expectation(description: "Capture cancellation")
        view.onError = { message in XCTAssertTrue(message.contains("focus")); done.fulfill() }
        view.onSwipe = { _ in XCTFail("Should not return a partial swipe after focus loss") }
        view.setArmed(true, generation: 1)
        _ = view.resignFirstResponder()
        await fulfillment(of: [done], timeout: 2)
    }

    @MainActor func testRenderMainScreens() throws {
        _ = NSApplication.shared
        let model = AppModel(scanDevices: false)
        let output = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks")
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        for page in [AppModel.AppPage.write, .read, .nfc, .help] {
            model.selectedPage = page
            if page == .read {
                model.armKeyboard()
                model.acceptKeyboard("%MIDWEST TEST 12345?;1234567890=12345?+9876543210=54321?\r")
            }
            if page == .nfc {
                model.nfc.hfPage = .tools
                model.nfc.batchPresented = true
                model.nfc.profileOrigin = "Sample credentials.qcard"
                let card = NFCCardInfo(uid: [1,2,3,4], atr: [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1,0,0,0,0,0x6A])
                let snapshot = ClassicSnapshot(card: card, blocks: Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8($0), count: 16)) }))
                model.nfc.profile = try CardCredentials(snapshot: snapshot)
                model.nfc.copiesSucceeded = 1; model.nfc.copiesFailed = 1
                model.nfc.copyResults = [NFCCopyResult(number: 2, success: true, detail: "47 blocks verified · ID 01 02 03 04"), NFCCopyResult(number: 1, success: false, detail: "Authentication failed. Check the target card key.")]
            }
            let host = NSHostingView(rootView: ContentView().environmentObject(model))
            host.frame = NSRect(x: 0, y: 0, width: 1180, height: 900)
            let window = NSWindow(contentRect: host.frame, styleMask: .borderless, backing: .buffered, defer: false)
            window.contentView = host
            host.layoutSubtreeIfNeeded()
            guard let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
                XCTFail("Could not render the app view"); continue
            }
            host.cacheDisplay(in: host.bounds, to: bitmap)
            let data = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
            XCTAssertGreaterThan(data.count, 5000)
            try data.write(to: output.appendingPathComponent("\(page == .write ? "write" : page == .read ? "read" : page == .nfc ? "nfc" : "help").png"))
            window.contentView = nil
        }
    }

    @MainActor func testRenderHFRecordsAndEditors() throws {
        _ = NSApplication.shared
        let model = AppModel(scanDevices: false)
        model.selectedPage = .nfc
        model.nfc.readers = ["ACS ACR122U PICC Interface"]; model.nfc.selectedReader = model.nfc.readers[0]
        let card = NFCCardInfo(uid: [0xCB,0xE7,0x22,0x58], atr: [0x3B,0x8F,0x80,1,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1,0,0,0,0,0x6A])
        var blocks = Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8(0), count: 16)) })
        var mad: [UInt8] = [0,1] + Array(repeating: [UInt8(3),0xE1], count: 15).flatMap { $0 }
        mad[0] = ClassicNDEF.crc(Array(mad.dropFirst()))
        blocks[1] = Array(mad.prefix(16)); blocks[2] = Array(mad.suffix(16))
        blocks[3]![9] = 0xC1
        for sector in 1..<16 { blocks[sector*4+3] = [0,0,0,0,0,0,0x7F,0x07,0x88,0x40,255,255,255,255,255,255] }
        let message = try NDEFCodec.encode([.uri("https://midwestgadgets.org")])
        let area = [3,UInt8(message.count)] + message + [0xFE] + Array(repeating: UInt8(0), count: 720-message.count-3)
        for (index, block) in (4..<64).filter({ $0 % 4 != 3 }).enumerated() { blocks[block] = Array(area[index*16..<index*16+16]) }
        let snapshot = ClassicSnapshot(card: card, blocks: blocks)
        model.nfc.snapshot = snapshot; model.nfc.inspectRecords(snapshot)
        model.nfc.status = "Read complete — 1 NFC record."; model.nfc.statusKind = .success
        model.nfc.recordQueue = [.uri("https://midwestgadgets.org"), try .text("Welcome to Midwest Gadgets")]
        XCTAssertNil(model.nfc.recordWriteError)
        let output = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks")
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        func render<V: View>(_ view: V, name: String, width: CGFloat, height: CGFloat) throws {
            let host = NSHostingView(rootView: view)
            host.frame = NSRect(x: 0, y: 0, width: width, height: height)
            let window = NSWindow(contentRect: host.frame, styleMask: .borderless, backing: .buffered, defer: false)
            window.contentView = host; host.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.08))
            host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            let data = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
            try data.write(to: output.appendingPathComponent(name + ".png")); window.contentView = nil
        }
        for page in [NFCModel.HFPage.read, .write, .advanced] {
            model.nfc.hfPage = page
            try render(ContentView().environmentObject(model), name: "hf-" + page.rawValue, width: 1100, height: 760)
        }
        for kind in [RecordKind.contact, .wifi, .filament] {
            try render(NFCRecordEditor(initialKind: kind) { _ in }, name: "editor-" + kind.rawValue, width: 616, height: 646)
        }
        model.nfc.recordQueue = [try .text(String(repeating:"x",count:800))]
        XCTAssertNotNil(model.nfc.recordWriteError)
    }
}
