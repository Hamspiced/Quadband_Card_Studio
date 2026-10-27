// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import XCTest
import MagStripeCore
@testable import MagStripeStudio

final class NFCCopyTests: XCTestCase {
    @MainActor func testBatchContinuesAfterFailureAndRequiresRemoval() async throws {
        let service = CopyFake(presence: [true,true,false,true,true,false,true], failFirst: true)
        let model = NFCModel(service: service, pollingInterval: 1_000_000)
        model.selectedReader = "Test ACS reader"; model.profile = try copyProfile()
        model.startBatch()
        for _ in 0..<300 {
            if model.copyResults.count == 2 { break }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTAssertTrue(model.batchRunning)
        XCTAssertEqual(model.copiesFailed, 1)
        XCTAssertEqual(model.copiesSucceeded, 1)
        XCTAssertEqual(model.copyResults.map(\.success), [true,false])
        // Same UID on two cards is accepted, but leaving the second card present does not repeat it.
        try await Task.sleep(nanoseconds: 30_000_000)
        let writes = await service.writeCount
        XCTAssertEqual(writes, 2)
        model.stopBatch()
        for _ in 0..<100 {
            if !model.busy { break }
            try await Task.sleep(nanoseconds: 2_000_000)
        }
        XCTAssertFalse(model.busy)
        XCTAssertFalse(model.batchRunning)
        XCTAssertTrue(model.status.contains("1 succeeded; 1 failed"))
    }
    @MainActor func testBatchNeverWritesSourceCardUntilRemoved() async throws {
        let service = CopyFake(presence: [true], failFirst: false)
        let model = NFCModel(service: service, pollingInterval: 1_000_000)
        model.selectedReader = "Test ACS reader"; model.profile = try copyProfile()
        model.startBatch()
        try await Task.sleep(nanoseconds: 30_000_000)
        let writes = await service.writeCount
        XCTAssertEqual(writes, 0)
        XCTAssertTrue(model.status.contains("Remove"))
        model.stopBatch()
        try await Task.sleep(nanoseconds: 5_000_000)
        XCTAssertFalse(model.busy)
    }
}

private let copyCard = NFCCardInfo(uid: [1,2,3,4], atr: [0x3B,0x8F,0x80,0x01,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,1,0,0,0,0,0x6A])
private func copyProfile() throws -> CardCredentials {
    try CardCredentials(snapshot: ClassicSnapshot(card: copyCard, blocks: Dictionary(uniqueKeysWithValues: (0..<64).map { ($0, Array(repeating: UInt8($0), count: 16)) })))
}

private actor CopyFake: NFCServicing {
    var presence: [Bool]
    let failFirst: Bool
    var writeCount = 0
    init(presence: [Bool], failFirst: Bool) { self.presence = presence; self.failFirst = failFirst }
    func readers() -> [String] { ["Test ACS reader"] }
    func identify(reader: String) -> NFCCardInfo { copyCard }
    func read(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) -> ClassicSnapshot { ClassicSnapshot(card: copyCard) }
    func readAutomatic(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) -> ClassicSnapshot { ClassicSnapshot(card: copyCard) }
    func writeNDEF(reader: String, records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) throws -> ClassicSnapshot { throw StripeError("Not part of this copy test") }
    func write(reader: String, block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) -> [UInt8] { data }
    func isPresent(reader: String) -> Bool { presence.count > 1 ? presence.removeFirst() : presence.first ?? false }
    func writeProfile(reader: String, profile: CardCredentials, key: [UInt8], type: ClassicKeyType) throws -> NFCCardInfo {
        writeCount += 1
        if failFirst && writeCount == 1 { throw StripeError("Simulated authentication failure") }
        return copyCard
    }
}
