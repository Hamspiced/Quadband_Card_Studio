import XCTest
@testable import MagStripeCore

final class NDEFBulkTests: XCTestCase {
    func testClassicReadCompletionAndWriteVerificationRecheckIdentity() throws {
        for automatic in [false,true] {
            for sector: Int? in [nil,15] {
                let device = NativeNDEFDevice(.classic1k,blank:true)
                device.swapAfterRead = 63
                let client = ClassicClient(transport:device)
                XCTAssertThrowsError(try automatic ?
                    client.readAutomatic(key:NDEFFormatting.factory,type:.a,sector:sector) :
                    client.read(key:NDEFFormatting.factory,type:.a,sector:sector))
                XCTAssertTrue(device.writes.isEmpty)
            }
        }
        for afterMutation in [false,true] {
            let device = NativeNDEFDevice(.classic1k,blank:true)
            let client = ClassicClient(transport:device)
            let snapshot = try client.read(key:NDEFFormatting.factory,type:.a)
            if afterMutation { device.swapAfterWrite = true } else { device.swapAfterRead = 4 }
            XCTAssertThrowsError(try client.write(block:4,data:Array(repeating:1,count:16),
                key:NDEFFormatting.factory,type:.a,expectedCard:snapshot.card,previous:snapshot.blocks[4]!)) {
                if afterMutation { XCTAssertTrue($0.localizedDescription.contains("could not be verified")) }
            }
            XCTAssertEqual(device.writes.count,afterMutation ? 1 : 0)
        }
    }
    func testHiddenDataAndCanonicalCapacityLabels() throws {
        let record = try NDEFRecord.text("new")
        var target = bulkSnapshot(.ntag215)
        target.blocks[50] = [0x4B,0x45,0x45,0x50]
        XCTAssertThrowsError(try NDEFBulkJob(records:[record],family:.ntag215).review(target))
        XCTAssertNoThrow(try NDEFBulkJob(records:[record],family:.ntag215,overwrite:true).review(target))
        for (family,capacity) in [(CardFamily.ntag213,142),(.ntag215,492),(.ntag216,868)] {
            XCTAssertEqual(try NDEFBulkJob(records:[record],family:family).canonicalCapacity,capacity)
            var blank = bulkSnapshot(family,blank:true)
            blank.blocks[family.dynamicLockPage]![3] = 0xBD
            XCTAssertNoThrow(try NDEFFormatting.snapshotBytes(blank,overwrite:false))
            if family != .ntag213 {
                let outside = 4+Int(try NDEFFormatting.cc(family)[2])*2
                blank.blocks[outside] = [0x4B,0x45,0x45,0x50]
                XCTAssertThrowsError(try NDEFFormatting.snapshotBytes(blank,overwrite:false))
                XCTAssertNoThrow(try NDEFFormatting.snapshotBytes(blank,overwrite:true))
            }
        }
        for gpb: UInt8 in [0xC1,0xC2] {
            var classic = bulkSnapshot(.classic1k,blank:true)
            classic.blocks[3]![9] = gpb
            XCTAssertThrowsError(try NDEFFormatting.snapshotBytes(classic,overwrite:true))
        }
    }
    func testFrozenContractFixtureAndCanonicalScope() throws {
        let root = URL(fileURLWithPath:#filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let fixture = try XCTUnwrap(JSONSerialization.jsonObject(with:Data(contentsOf:root.appendingPathComponent("protocol/fixtures/ndef-bulk-v1.json"))) as? [String:Any])
        XCTAssertEqual(fixture["bulkLimit"] as? Int,3)
        XCTAssertEqual(fixture["families"] as? [String],NDEFBulkJob.families.map(\.rawValue))
        let formats = try XCTUnwrap(fixture["formatOperations"] as? [String:[String:Any]])
        for family in NDEFBulkJob.families {
            let snapshot = bulkSnapshot(family,blank:true)
            XCTAssertEqual(try NDEFFormatting.snapshotBytes(snapshot,overwrite:false).count,formats[family.rawValue]?["snapshotBytes"] as? Int)
            let units = try NDEFFormatting.canonicalUnits(family)
            if family.isNTAG {
                XCTAssertEqual(try NDEFFormatting.cc(family).map { String(format:"%02x",$0) }.joined(),formats[family.rawValue]?["cc"] as? String)
                XCTAssertTrue(units.allSatisfy { $0.0 == 3 || (4..<4+Int(try! NDEFFormatting.cc(family)[2])*2).contains($0.0) })
                XCTAssertEqual(units.last?.0,3)
            } else {
                XCTAssertFalse(units.contains { $0.0 == 0 })
                XCTAssertEqual(units.suffix(3).map(\.0),[1,2,3])
                XCTAssertEqual(units.last?.1,NDEFFormatting.directoryTrailer)
            }
        }
    }
    func testBulkLimitsAreNotDecoderOrSingleTargetLimitsAndUTF8IsSerialized() throws {
        let records = try (0..<4).map { try NDEFRecord.text("é🌎\($0)") }
        XCTAssertNoThrow(try NDEFBulkJob(records:Array(records.prefix(3)),family:.ntag213))
        XCTAssertThrowsError(try NDEFBulkJob(records:records,family:.ntag216))
        XCTAssertThrowsError(try NDEFBulkJob(records:[],family:.ntag213))
        XCTAssertThrowsError(try NDEFBulkJob(records:[records[0]],family:.classic4k))
        XCTAssertEqual(try NDEFCodec.decode(NDEFCodec.encode(records)),records)
        let job = try NDEFBulkJob(records:[records[0]],family:.ntag213)
        XCTAssertEqual(job.encodedBytes,try NDEFCodec.encode([records[0]]).count)
        XCTAssertFalse(job.overwrite); XCTAssertFalse(job.format)
    }
    func testExactFitOverflowAndTLVLengthBoundaries() throws {
        for size in [254,255] {
            let record = NDEFRecord(tnf:2,type:[0x78],payload:Array(repeating:0x41,count:size-4))
            XCTAssertEqual(try NDEFCodec.encode([record]).count,size)
            let area = try ClassicNDEF(snapshot:bulkSnapshot(.ntag215))
            let bytes = try area.replacing(with:[record])
            XCTAssertEqual(Array(bytes.prefix(size == 254 ? 2 : 4)),size == 254 ? [3,254] : [3,255,0,255])
        }
        let fit = NDEFRecord(tnf:2,type:[0x78],payload:Array(repeating:1,count:138))
        XCTAssertEqual(try NDEFCodec.encode([fit]).count,142)
        XCTAssertNoThrow(try NDEFBulkJob(records:[fit],family:.ntag213))
        let overflow = NDEFRecord(tnf:2,type:[0x78],payload:Array(repeating:1,count:139))
        XCTAssertThrowsError(try NDEFBulkJob(records:[overflow],family:.ntag213))
    }
    func testRemovalBarriersOneUseReviewGenerationAndUnverifiedLatch() throws {
        let job = try NDEFBulkJob(records:[.text("frozen")],family:.ntag213)
        let snapshot = bulkSnapshot(.ntag213)
        var state = NDEFBulkState(); state.arm(job)
        XCTAssertEqual(state.phase,.removalRequired)
        XCTAssertThrowsError(try state.inspect(snapshot))
        state.presence(true); XCTAssertEqual(state.phase,.removalRequired)
        state.presence(false); try state.inspect(snapshot)
        let review = try XCTUnwrap(state.review), generation = state.generation
        XCTAssertThrowsError(try state.confirm(review.id,generation:UUID()))
        _ = try state.confirm(review.id,generation:generation)
        XCTAssertThrowsError(try state.confirm(review.id,generation:generation))
        state.finish(); XCTAssertEqual(state.succeeded,1)
        XCTAssertThrowsError(try state.inspect(snapshot))
        state.presence(true); XCTAssertEqual(state.phase,.removalRequired)
        state.presence(false); try state.inspect(snapshot)
        _ = try state.confirm(state.review!.id,generation:state.generation)
        state.finish(unverified:true); state.stop(); state.arm(job)
        XCTAssertEqual(state.phase,.unverified); XCTAssertEqual(state.failed,1)
    }
    func testOverwriteFamilyCapacityAndFormattingPreflightAreIndependent() throws {
        var target = bulkSnapshot(.ntag213)
        let old = try NDEFCodec.encode([.text("existing")])
        install([3,UInt8(old.count)]+old+[254],in:&target)
        let records = [try NDEFRecord.text("new")]
        XCTAssertThrowsError(try NDEFBulkJob(records:records,family:.ntag213).review(target))
        XCTAssertFalse(try NDEFBulkJob(records:records,family:.ntag213,overwrite:true).review(target).requiresFormat)
        XCTAssertTrue(try NDEFBulkJob(records:[.text("existing")],family:.ntag213).review(target).alreadyCorrect)
        XCTAssertThrowsError(try NDEFBulkJob(records:records,family:.ntag215,overwrite:true).review(target))
        let blank = bulkSnapshot(.ntag213,blank:true)
        XCTAssertThrowsError(try NDEFBulkJob(records:records,family:.ntag213).review(blank))
        XCTAssertTrue(try NDEFBulkJob(records:records,family:.ntag213,format:true).review(blank).requiresFormat)
        for mutation in 0..<6 {
            var bad = blank
            switch mutation {
            case 0: bad.blocks.removeValue(forKey:20)
            case 1: bad.blocks[3] = [0xE1,0x20,0x12,0]
            case 2: bad.blocks[2]![2] = 1
            case 3: bad.blocks[40]![0] = 1
            case 4: bad.blocks[41]![3] = 4
            default: bad.card.version[6] = 0x11
            }
            XCTAssertThrowsError(try NDEFFormatting.snapshotBytes(bad,overwrite:true))
        }
        var classic = bulkSnapshot(.classic1k,blank:true)
        classic.blocks[63]![6] = 0
        XCTAssertThrowsError(try NDEFFormatting.snapshotBytes(classic,overwrite:true))
        classic = bulkSnapshot(.classic1k,blank:true); classic.sectorKeys[15] = nil
        XCTAssertThrowsError(try NDEFFormatting.snapshotBytes(classic,overwrite:true))
    }
    func testDirectPCSCVersionGeometryBatchedReadsAndLengthLastWrites() throws {
        for family in [CardFamily.ntag213,.ntag215,.ntag216] {
            let device = NativeNDEFDevice(family), client = ClassicClient(transport:device)
            let snapshot = try client.readAutomatic(key:[],type:.a)
            XCTAssertEqual(snapshot.card.family,family)
            XCTAssertEqual(snapshot.blocks.count,family.unitCount-2)
            XCTAssertNil(snapshot.blocks[family.unitCount-2])
            XCTAssertEqual(device.reads,(family.unitCount-2+3)/4)
            let protected = device.memory.filter { !family.isData($0.key) }
            let records = [try NDEFRecord.text("Hello é🌎")]
            let result = try client.writeNDEF(records,expected:ClassicNDEF(snapshot:snapshot),key:[],type:.a)
            XCTAssertEqual(try ClassicNDEF(snapshot:result).records,records)
            XCTAssertEqual(device.writes.first?.1[1],0)
            XCTAssertEqual(device.writes.last?.1[1],UInt8(try NDEFCodec.encode(records).count))
            XCTAssertEqual(device.memory.filter { !family.isData($0.key) },protected)
            for page in [0,2,3,family.dynamicLockPage,family.unitCount-1] {
                XCTAssertThrowsError(try client.write(block:page,data:[0,0,0,0],key:[],type:.a,expectedCard:snapshot.card,previous:[0,0,0,0]))
            }
        }
    }
    func testDirectFormatReadBackFactoryKeysAndNoProtectedGenericTunnel() throws {
        for family in NDEFBulkJob.families {
            let device = NativeNDEFDevice(family,blank:true), client = ClassicClient(transport:device)
            let snapshot = try client.read(key:NDEFFormatting.factory,type:.a)
            let original = device.memory
            let result = try client.formatNDEF(expected:snapshot)
            XCTAssertTrue(try ClassicNDEF(snapshot:result).records.isEmpty)
            XCTAssertEqual(device.memory[0],original[0])
            if family.isNTAG {
                let allowed = Set(try NDEFFormatting.canonicalUnits(family).map(\.0))
                XCTAssertEqual(device.memory.filter { !allowed.contains($0.key) },original.filter { !allowed.contains($0.key) })
            } else {
                XCTAssertEqual(device.memory[3],NDEFFormatting.directoryTrailer)
                XCTAssertEqual(device.memory[63],NDEFFormatting.dataTrailer)
                XCTAssertTrue(device.authenticatedKeys.contains(Array(NDEFFormatting.directoryTrailer.prefix(6))))
            }
        }
    }
    func testDirectFormatFailedPreflightStaleMetadataAndEachMutationFailureNeverRetries() throws {
        for family in [CardFamily.classic1k,.ntag213] {
            let device = NativeNDEFDevice(family,blank:true), client = ClassicClient(transport:device)
            let snapshot = try client.read(key:NDEFFormatting.factory,type:.a)
            device.memory[family.isClassic ? 62 : 20]![0] = 1
            XCTAssertThrowsError(try client.formatNDEF(expected:snapshot,overwrite:true)); XCTAssertTrue(device.writes.isEmpty)
            for stage in 1...(try NDEFFormatting.canonicalUnits(family).count) {
                let failing = NativeNDEFDevice(family,blank:true), writer = ClassicClient(transport:failing)
                let expected = try writer.read(key:NDEFFormatting.factory,type:.a)
                failing.failWrite = stage
                XCTAssertThrowsError(try writer.formatNDEF(expected:expected)) { XCTAssertTrue($0.localizedDescription.contains("unverified")) }
                XCTAssertEqual(failing.writes.count,stage)
            }
        }
    }
    func testUnsupportedVersionIncompleteReadAndPermissionsFailWithoutWrites() throws {
        let device = NativeNDEFDevice(.ntag213), client = ClassicClient(transport:device)
        device.badVersion = true; XCTAssertThrowsError(try client.identify()); device.badVersion = false
        device.shortRead = true; XCTAssertThrowsError(try client.read(key:[],type:.a)); device.shortRead = false
        let expected = try client.read(key:[],type:.a)
        device.memory[40]![0] = 1
        XCTAssertThrowsError(try client.write(block:16,data:[1,2,3,4],key:[],type:.a,expectedCard:expected.card,previous:expected.blocks[16]!))
        XCTAssertTrue(device.writes.isEmpty)
    }
}

func bulkSnapshot(_ family: CardFamily, blank: Bool = false) -> ClassicSnapshot {
    var card = NFCCardInfo(uid:family.isNTAG ? [4,1,2,3,4,5,6] : [1,2,3,4],atr:[])
    card.detectedFamily = family
    if family.isNTAG { card.version = [0,4,4,2,1,0,family == .ntag213 ? 15 : family == .ntag215 ? 17 : 19,3] }
    var snapshot = ClassicSnapshot(card:card,blocks:Dictionary(uniqueKeysWithValues:(0..<(family.isNTAG ? family.unitCount-2 : family.unitCount)).map { ($0,Array(repeating:UInt8(0),count:family.unitSize)) }))
    if family.isNTAG {
        snapshot.blocks[family.dynamicLockPage+1]![3] = 255
        if !blank { snapshot.blocks[3] = try! NDEFFormatting.cc(family); snapshot.blocks[4] = [3,0,254,0] }
    } else {
        for sector in 0..<family.sectorCount {
            snapshot.sectorKeys[sector] = ClassicCredential(bytes:NDEFFormatting.factory,type:.a)
            snapshot.blocks[family.blocks(in:sector).last!] = NDEFFormatting.factory + [255,7,128,0x69] + NDEFFormatting.factory
        }
    }
    return snapshot
}

private func install(_ bytes: [UInt8], in snapshot: inout ClassicSnapshot) {
    let capacity = Int(snapshot.blocks[3]![2])*8
    let padded = bytes + Array(repeating:UInt8(0),count:capacity-bytes.count)
    for index in 0..<capacity/4 { snapshot.blocks[4+index] = Array(padded[index*4..<index*4+4]) }
}

private final class NativeNDEFDevice: NFCTransport {
    let family: CardFamily
    var memory: [Int:[UInt8]]
    var writes: [(Int,[UInt8])] = []
    var reads = 0
    var loaded: [UInt8] = []
    var authenticatedKeys: [[UInt8]] = []
    var failWrite: Int?
    var uid: [UInt8]
    var swapAfterRead: Int?
    var swapAfterWrite = false
    var badVersion = false, shortRead = false
    var atr: [UInt8] { [0x3B,0x8F,0x80,1,0x80,0x4F,0x0C,0xA0,0,0,3,6,3,0,family.isNTAG ? 3 : 1,0,0,0,0,0] }
    init(_ family: CardFamily, blank: Bool = false) {
        self.family = family
        let snapshot = bulkSnapshot(family,blank:blank)
        memory = snapshot.blocks; uid = snapshot.card.uid
    }
    func resetAuthentication() {}
    func transmit(_ bytes: [UInt8]) throws -> [UInt8] {
        let unit = Int(bytes[3])
        switch bytes[1] {
        case 0xCA: return uid + [0x90,0]
        case 0:
            var version: [UInt8] = [0,4,4,2,1,0,family == .ntag213 ? 15 : family == .ntag215 ? 17 : 19,3]
            if badVersion { version[1] = 0 }
            XCTAssertEqual(bytes,[255,0,0,0,4,0xD4,0x40,1,0x60])
            return [0xD5,0x41,0]+version+[0x90,0]
        case 0x82: loaded = Array(bytes.dropFirst(5)); return [0x90,0]
        case 0x86:
            let trailer = memory[Int(bytes[7])/4*4+3]!
            guard loaded == Array(trailer.prefix(6)) else { return [0x63,0] }
            authenticatedKeys.append(loaded); return [0x90,0]
        case 0xB0:
            reads += 1
            if shortRead { return [0x90,0] }
            if family.isNTAG { return (unit..<unit+Int(bytes[4])/4).flatMap { memory[$0]! } + [0x90,0] }
            var value = memory[unit]!
            if unit == swapAfterRead { uid = [4,3,2,1]; swapAfterRead = nil }
            if unit % 4 == 3 {
                value.replaceSubrange(0..<6,with:Array(repeating:0,count:6))
                if memory[unit]![9] == 0x40 || memory[unit]![9] == 0xC1 {
                    value.replaceSubrange(10..<16,with:Array(repeating:0,count:6))
                }
            }
            return value + [0x90,0]
        case 0xD6:
            let value = Array(bytes.dropFirst(5)); writes.append((unit,value))
            if writes.count == failWrite { return [0x63,0] }
            memory[unit] = value
            if swapAfterWrite { uid = [4,3,2,1] }
            return [0x90,0]
        default: throw StripeError("Unexpected simulated APDU")
        }
    }
}
