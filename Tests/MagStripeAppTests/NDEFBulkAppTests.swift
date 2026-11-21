import XCTest
import SwiftUI
import AppKit
import MagStripeCore
@testable import MagStripeStudio

final class NDEFBulkAppTests: XCTestCase {
    @MainActor func testRenderOfflineBulkWriter() throws {
        _ = NSApplication.shared
        let model = makeModel(BulkService())
        model.bulkFormat = true
        let host = NSHostingView(rootView:NFCRecordsWriteView(model:model))
        host.frame = NSRect(x:0,y:0,width:1000,height:1000)
        let window = NSWindow(contentRect:host.frame,styleMask:.borderless,backing:.buffered,defer:false)
        window.contentView = host; host.layoutSubtreeIfNeeded()
        RunLoop.current.run(until:Date().addingTimeInterval(0.08))
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in:host.bounds))
        host.cacheDisplay(in:host.bounds,to:bitmap)
        let data = try XCTUnwrap(bitmap.representation(using:.png,properties:[:]))
        XCTAssertGreaterThan(data.count,5000)
        let url = URL(fileURLWithPath:FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks/ndef-bulk-writer.png")
        try FileManager.default.createDirectory(at:url.deletingLastPathComponent(),withIntermediateDirectories:true)
        try data.write(to:url); window.contentView = nil
    }
    @MainActor func testHeldInitialTargetAndPollingReviewNeverWrite() async throws {
        let service = BulkService(), model = makeModel(service)
        model.armNDEFBulk()
        try await Task.sleep(nanoseconds:15_000_000)
        XCTAssertEqual(model.ndefBulk.phase,.removalRequired)
        await assertWrites(service,0)
        await service.setPresence(false)
        try await wait { model.ndefBulk.phase == .ready }
        await service.setPresence(true)
        try await wait { model.ndefBulk.phase == .review }
        try await Task.sleep(nanoseconds:15_000_000)
        await assertWrites(service,0)
        XCTAssertFalse(model.batchRunning)
        XCTAssertTrue(model.busy)
        model.stopNDEFBulk(); XCTAssertFalse(model.busy)
    }
    @MainActor func testFreshConfirmationOneUseAndSubsequentRemoval() async throws {
        let service = BulkService(), model = makeModel(service)
        try await prepareReview(model,service)
        let review = try XCTUnwrap(model.ndefBulk.review), generation = model.ndefBulk.generation
        try await wait { !model.bulkChecking }
        model.confirmNDEFBulk(review.id,generation:generation)
        try await wait { !model.writing }
        XCTAssertEqual(model.ndefBulk.succeeded,1)
        await assertWrites(service,1)
        model.confirmNDEFBulk(review.id,generation:generation)
        try await Task.sleep(nanoseconds:10_000_000)
        await assertWrites(service,1)
        XCTAssertEqual(model.ndefBulk.phase,.removalRequired)
        await service.setPresence(false)
        try await wait { model.ndefBulk.phase == .ready }
        await service.setPresence(true)
        try await wait { model.ndefBulk.phase == .review }
        XCTAssertNotEqual(model.ndefBulk.review?.id,review.id)
        await assertWrites(service,1)
        model.stopNDEFBulk()
    }
    @MainActor func testDraftCredentialsReaderAndStopInvalidateReviews() async throws {
        for mutation in 0..<6 {
            let service = BulkService(), model = makeModel(service)
            try await prepareReview(model,service)
            let review = try XCTUnwrap(model.ndefBulk.review), generation = model.ndefBulk.generation
            switch mutation {
            case 0: model.recordQueue = [try .text("changed")]
            case 1: model.key = "00 00 00 00 00 00"
            case 2: model.selectedReader = "Different reader"
            case 3: model.bulkOverwrite = true
            case 4: model.bulkFamily = .ntag215
            default: model.stopNDEFBulk()
            }
            XCTAssertNil(model.ndefBulk.review); XCTAssertNotEqual(model.ndefBulk.generation,generation)
            model.confirmNDEFBulk(review.id,generation:generation)
            await assertWrites(service,0)
        }
    }
    @MainActor func testErrorIsNotRemovalAndLostWriteLatchesWithoutRetry() async throws {
        let service = BulkService(), model = makeModel(service)
        await service.failPresence()
        model.armNDEFBulk()
        try await Task.sleep(nanoseconds:15_000_000)
        XCTAssertEqual(model.ndefBulk.phase,.removalRequired)
        await assertWrites(service,0)
        model.stopNDEFBulk()

        let lost = BulkService(), writer = makeModel(lost)
        try await prepareReview(writer,lost)
        await lost.loseWrite()
        let review = try XCTUnwrap(writer.ndefBulk.review)
        try await wait { !writer.bulkChecking }
        writer.confirmNDEFBulk(review.id,generation:writer.ndefBulk.generation)
        try await wait { writer.ndefBulk.phase == .unverified && !writer.writing }
        await assertWrites(lost,1)
        writer.armNDEFBulk(); try await Task.sleep(nanoseconds:15_000_000)
        await assertWrites(lost,1); XCTAssertEqual(writer.ndefBulk.phase,.unverified)
        XCTAssertFalse(writer.busy)
    }
    @MainActor func testChangedTargetBeforeConfirmationDoesNotWrite() async throws {
        let service = BulkService(), model = makeModel(service)
        try await prepareReview(model,service)
        let review = try XCTUnwrap(model.ndefBulk.review), generation = model.ndefBulk.generation
        await service.changeData()
        try await wait { !model.bulkChecking }
        model.confirmNDEFBulk(review.id,generation:generation)
        try await wait { !model.writing }
        await assertWrites(service,0)
        XCTAssertEqual(model.ndefBulk.phase,.unverified)
    }
    @MainActor func testFormattingGateDoesNotEnableChameleonOrLegacyBridge() {
        let model = AppModel(scanDevices:false).nfc
        XCTAssertFalse(model.formattingFamily("Chameleon Ultra · USB",.classic1k))
        XCTAssertFalse(model.formattingFamily("CoreS3 · HF",.ntag213))
        XCTAssertTrue(model.formattingFamily("ACS ACR122U PICC Interface",.ntag213))
    }
    private func assertWrites(_ service: BulkService, _ expected: Int, file: StaticString = #filePath, line: UInt = #line) async {
        let actual = await service.count()
        XCTAssertEqual(actual,expected,file:file,line:line)
    }
    @MainActor private func makeModel(_ service: BulkService) -> NFCModel {
        let model = NFCModel(service:service,pollingInterval:2_000_000)
        model.selectedReader = "Test ACS reader"; model.bulkFamily = .ntag213
        model.writableFamily = { _,family in family.isNTAG }
        model.recordQueue = [.uri("https://example.org")]
        return model
    }
    @MainActor private func prepareReview(_ model: NFCModel, _ service: BulkService) async throws {
        model.armNDEFBulk()
        await service.setPresence(false)
        try await wait { model.ndefBulk.phase == .ready }
        await service.setPresence(true)
        try await wait { model.ndefBulk.phase == .review }
    }
    @MainActor private func wait(_ condition: () -> Bool) async throws {
        for _ in 0..<300 {
            if condition() { return }
            try await Task.sleep(nanoseconds:1_000_000)
        }
        XCTFail("Timed out waiting for native bulk state")
    }
}

private actor BulkService: NFCServicing {
    var present = true, presenceError = false, writeError = false
    var writes = 0
    var snapshot: ClassicSnapshot
    init() {
        var card = NFCCardInfo(uid:[4,1,2,3,4,5,6],atr:[])
        card.detectedFamily = .ntag213; card.version = [0,4,4,2,1,0,15,3]
        snapshot = ClassicSnapshot(card:card,blocks:Dictionary(uniqueKeysWithValues:(0..<43).map { ($0,[UInt8](repeating:0,count:4)) }))
        snapshot.blocks[3] = [0xE1,0x10,0x12,0]; snapshot.blocks[4] = [3,0,254,0]; snapshot.blocks[41]![3] = 255
    }
    func setPresence(_ value: Bool) { present = value }
    func failPresence() { presenceError = true }
    func loseWrite() { writeError = true }
    func changeData() { snapshot.blocks[20]![0] ^= 1 }
    func count() -> Int { writes }
    func readers() -> [String] { ["Test ACS reader"] }
    func identify(reader: String) -> NFCCardInfo { snapshot.card }
    func read(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) -> ClassicSnapshot { snapshot }
    func readAutomatic(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) -> ClassicSnapshot { snapshot }
    func isPresent(reader: String) throws -> Bool {
        if presenceError { throw StripeError("Transport error is not absence") }
        return present
    }
    func write(reader: String, block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) throws -> [UInt8] { throw StripeError("Generic writes not part of bulk test") }
    func writeProfile(reader: String, profile: CardCredentials, key: [UInt8], type: ClassicKeyType) throws -> NFCCardInfo { throw StripeError("Copy workflow must remain separate") }
    func writeNDEF(reader: String, records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) throws -> ClassicSnapshot {
        writes += 1
        if writeError { throw StripeError("Lost response; target unverified") }
        let bytes = try expected.replacing(with:records)
        for (index,page) in expected.blocks.enumerated() { snapshot.blocks[page] = Array(bytes[index*4..<index*4+4]) }
        return snapshot
    }
}
