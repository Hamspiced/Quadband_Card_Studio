import XCTest
@testable import MagStripeCore

enum BridgeFixtures {
    static var directory: URL {
        if let path = ProcessInfo.processInfo.environment["CORES3_PROTOCOL_DIR"] { return URL(fileURLWithPath:path) }
        return URL(fileURLWithPath:#filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("protocol")
    }
    static func object(_ file: String = "fixtures/cores3-v1.json") throws -> [String:Any] {
        try XCTUnwrap(JSONSerialization.jsonObject(with:Data(contentsOf:directory.appendingPathComponent(file))) as? [String:Any])
    }
    static func bytes(_ string: String) throws -> [UInt8] { try HexBytes.parse(string,count:string.count/2) }
}

final class CoreS3ProtocolTests: XCTestCase {
    func testInvalidModeMetadataIsRejected() throws {
        let base = try XCTUnwrap(BridgeFixtures.object()["capabilities"] as? [String:Any])
        for change: [String:Any] in [["mode":"invalid"],["mode":"home","owner":"desktop"],
                                    ["mode":"standalone","owner":"desktop"],["owner":"local"],
                                    ["modePending":true],["mode":"connected","modePending":"false"]] {
            let data = try JSONSerialization.data(withJSONObject:base.merging(change) { _,new in new })
            XCTAssertThrowsError(try JSONDecoder().decode(CoreS3Capabilities.self,from:data).validated())
        }
    }
    func testUnknownNonNULLNTAGAndBlankClassicAreNotSilentlyFormatted() throws {
        var card = NFCCardInfo(uid:[4,1,2,3,4,5,6],atr:[]); card.detectedFamily = .ntag213
        var snapshot = ClassicSnapshot(card:card,blocks:Self.permissionPages(.ntag213))
        for page in CardFamily.ntag213.userPages { snapshot.blocks[page] = [0,0,0,0] }
        XCTAssertNoThrow(try ClassicNDEF(snapshot:snapshot))
        snapshot.blocks[4] = [0xFD,0,0xFE,0]
        XCTAssertThrowsError(try ClassicNDEF(snapshot:snapshot))
        card.detectedFamily = .classic1k
        snapshot = ClassicSnapshot(card:card,blocks:Dictionary(uniqueKeysWithValues:(0..<64).map { ($0,Array(repeating:UInt8(0),count:16)) }))
        XCTAssertThrowsError(try ClassicNDEF(snapshot:snapshot))
    }
    func testSharedNTAGLockVectorsAndControlAddresses() throws {
        let root = try BridgeFixtures.object()
        for entry in root["ntagLockCases"] as! [[String:Any]] {
            let family = CardFamily(rawValue:entry["family"] as! String)!
            var pages = Self.permissionPages(family)
            pages[2] = [0,0] + (try BridgeFixtures.bytes(entry["staticLockHex"] as! String))
            pages[family.dynamicLockPage] = try BridgeFixtures.bytes(entry["dynamicLockPageHex"] as! String)
            let locked = Set(entry["lockedUserPages"] as! [Int])
            for page in family.userPages {
                let writable = (try? NTAGPermissions(family:family,pages:pages).requireWritable(page,authenticated:false)) != nil
                XCTAssertEqual(writable,!locked.contains(page),"\(entry["name"]!) page \(page)")
            }
        }
        for entry in root["ntagLockControlCases"] as! [[String:Any]] {
            let descriptor = try BridgeFixtures.bytes(entry["lockControlHex"] as! String)
            let address = Int(descriptor[0] >> 4) * (1 << Int(descriptor[2] & 15)) + Int(descriptor[0] & 15)
            XCTAssertEqual(address,entry["lockByteAddress"] as? Int)
        }
    }
    func testSharedStandardMAD2GoldenMemoryIncludesLargeSectors() throws {
        let object = try XCTUnwrap(BridgeFixtures.object()["classic4kNdef"] as? [String:Any])
        let card = NFCCardInfo(uid:try BridgeFixtures.bytes(object["uidHex"] as! String),atr:[],
                              atqa:try BridgeFixtures.bytes(object["atqaHex"] as! String),sak:UInt8(object["sak"] as! Int))
        var snapshot = ClassicSnapshot(card:card)
        for entry in object["blocks"] as! [[String:Any]] { snapshot.blocks[entry["number"] as! Int] = try BridgeFixtures.bytes(entry["hex"] as! String) }
        for sector in 0..<40 { snapshot.sectorKeys[sector] = ClassicCredential(bytes:Array(repeating:255,count:6),type:.a) }
        XCTAssertEqual(snapshot.blocks[3]?[9],0xC2); XCTAssertEqual(snapshot.blocks[67]?[9],0)
        let ndef = try ClassicNDEF(snapshot:snapshot)
        XCTAssertEqual(ndef.blocks,object["ndefBlocks"] as? [Int])
        XCTAssertFalse(ndef.blocks.contains { (64...67).contains($0) })
        XCTAssertEqual(try NDEFCodec.encode(ndef.records),try BridgeFixtures.bytes(object["messageHex"] as! String))
        XCTAssertTrue(ndef.blocks.contains(128)); XCTAssertTrue(ndef.blocks.contains(254))
        snapshot.blocks[64]?[0] ^= 1
        XCTAssertThrowsError(try ClassicNDEF(snapshot:snapshot))
    }
    func testManifestAndEveryGoldenFrameAtEveryFragmentBoundary() throws {
        let manifest = try BridgeFixtures.object("cores3-bridge-v1.json")
        let uuids = try XCTUnwrap(manifest["uuids"] as? [String:String])
        XCTAssertEqual(uuids,["service":CoreS3Protocol.service,"rx":CoreS3Protocol.rx,"tx":CoreS3Protocol.tx])
        XCTAssertEqual(manifest["maxPayload"] as? Int,CoreS3Protocol.maxPayload)
        XCTAssertEqual(manifest["headerSize"] as? Int,26)
        let entries = try XCTUnwrap(BridgeFixtures.object()["frames"] as? [[String:Any]])
        var joined: [UInt8] = [], expected: [CoreS3Frame] = []
        for entry in entries {
            let frame = CoreS3Frame(kind:UInt8(entry["kind"] as! Int),session:UInt64(entry["session"] as! String)!,
                requestID:UInt32(entry["requestId"] as! Int),channel:UInt8(entry["channel"] as! Int),
                operation:UInt8(entry["operation"] as! Int),status:UInt16(entry["status"] as! Int),
                generation:UInt32(entry["generation"] as! Int),payload:try BridgeFixtures.bytes(entry["payloadHex"] as! String))
            let bytes = try BridgeFixtures.bytes(entry["frameHex"] as! String)
            XCTAssertEqual(try frame.encoded(),bytes,entry["name"] as! String)
            for boundary in 1..<bytes.count {
                var decoder = CoreS3Decoder()
                XCTAssertEqual(try decoder.accept(Array(bytes.prefix(boundary))),[])
                XCTAssertEqual(try decoder.accept(Array(bytes.dropFirst(boundary))),[frame])
            }
            joined += bytes; expected.append(frame)
        }
        var decoder = CoreS3Decoder()
        XCTAssertEqual(try decoder.accept(joined),expected)
    }
    func testMalformedFramingPoisonsDecoderRatherThanResynchronizing() throws {
        let frame = try CoreS3Frame(session:1,requestID:1,channel:0,operation:3,payload:[0x51,0x42]).encoded()
        for index in [0,1,2,3,frame.count-1] {
            var corrupt = frame
            corrupt[index] = index == 3 ? 3 : corrupt[index] ^ 0xFF
            var decoder = CoreS3Decoder()
            XCTAssertThrowsError(try decoder.accept(corrupt + frame))
            XCTAssertThrowsError(try decoder.accept(frame))
        }
        var length = frame; length[24] = 1; length[25] = 8
        var decoder = CoreS3Decoder()
        XCTAssertThrowsError(try decoder.accept(length))
        decoder = CoreS3Decoder()
        XCTAssertThrowsError(try decoder.accept(Array(repeating:0,count:8313)))
        XCTAssertThrowsError(try CoreS3Frame(session:1,requestID:1,channel:0,operation:3,payload:Array(repeating:0,count:2049)).encoded())
    }
    func testGoldenGeometryAndProfilesIncludingLegacyEncoding() throws {
        let root = try BridgeFixtures.object(), geometries = try XCTUnwrap(root["geometries"] as? [String:Any])
        for family in [CardFamily.classic1k,.classic4k] {
            for sector in geometries[family.rawValue] as! [[String:Any]] {
                let n = sector["sector"] as! Int, first = sector["firstBlock"] as! Int, count = sector["blockCount"] as! Int
                XCTAssertEqual(family.firstBlock(n),first)
                XCTAssertEqual(Array(family.blocks(in:n)),Array(first..<first+count))
                XCTAssertTrue(family.isTrailer(sector["trailer"] as! Int))
                for (group,blocks) in (sector["accessGroups"] as! [[Int]]).enumerated() {
                    for block in blocks { XCTAssertEqual(family.accessGroup(block),group); XCTAssertEqual(family.sector(of:block),n) }
                }
            }
        }
        XCTAssertEqual(CardFamily.classic1k.dataUnits.count,47)
        XCTAssertEqual(CardFamily.classic4k.dataUnits.count,215)
        for family in [CardFamily.ntag213,.ntag215,.ntag216] {
            let entry = geometries[family.rawValue] as! [String:Int]
            XCTAssertEqual(family.unitCount,entry["unitCount"])
            XCTAssertEqual(family.unitSize,entry["unitSize"])
            XCTAssertEqual(family.userPages.lowerBound,entry["firstUserPage"])
            XCTAssertEqual(family.userPages.last,entry["lastUserPage"])
            XCTAssertEqual(family.dynamicLockPage,entry["dynamicLockPage"])
        }
        let profiles = (root["profiles"] as! [[String:Any]]) + (root["legacyProfiles"] as? [[String:Any]] ?? [])
        for object in profiles {
            let data = try JSONSerialization.data(withJSONObject:object), profile = try CardCredentials.decode(data)
            XCTAssertEqual(try profile.validatedBlocks().count,profile.cardFamily.dataUnits.count)
            let roundtrip = try XCTUnwrap(JSONSerialization.jsonObject(with:profile.encoded()) as? NSDictionary)
            XCTAssertEqual(roundtrip,object as NSDictionary)
            var invalid = profile; invalid.blocks[0] = invalid.blocks[1]
            XCTAssertThrowsError(try invalid.validatedBlocks())
            invalid = profile; invalid.geometry = .init(unitSize:4,unitCount:1)
            XCTAssertThrowsError(try invalid.validatedBlocks())
        }
    }
    func testLargeSectorAccessGroupsAndComplementValidation() throws {
        var trailer = Array(repeating:UInt8(0xFF),count:6) + [0xFF,0x07,0x80,0x69] + Array(repeating:UInt8(0xFF),count:6)
        for block in [128,132,133,137,138,142,240,254] {
            XCTAssertTrue(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.a))
        }
        for block in [0,3,127,143,255,256] { XCTAssertFalse(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.a)) }
        trailer[6] ^= 1
        XCTAssertThrowsError(try ClassicAccess.writable(block:128,family:.classic4k,trailer:trailer,key:.a))
    }
    func testLargeSectorPermissionGroupsHaveFiveDataBlocksEach() throws {
        var trailer = Array(repeating:UInt8(0),count:16)
        for (index,code) in [0,7,4,1].enumerated() {
            let c1 = UInt8(code >> 2), c2 = UInt8((code >> 1) & 1), c3 = UInt8(code & 1)
            trailer[6] |= ((1-c1) << index) | ((1-c2) << (4+index))
            trailer[7] |= ((1-c3) << index) | (c1 << (4+index))
            trailer[8] |= (c2 << index) | (c3 << (4+index))
        }
        for block in 128...132 { XCTAssertTrue(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.a)) }
        for block in 133...137 {
            XCTAssertFalse(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.a))
            XCTAssertFalse(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.b))
        }
        for block in 138...142 {
            XCTAssertFalse(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.a))
            XCTAssertTrue(try ClassicAccess.writable(block:block,family:.classic4k,trailer:trailer,key:.b))
        }
    }
    func testNTAGLockGranularityAuthAndPhysicalCCBounds() throws {
        for family in [CardFamily.ntag213,.ntag215,.ntag216] {
            var pages = Self.permissionPages(family)
            pages[2] = [0,0,7,0]
            pages[family.dynamicLockPage] = [0,0,0,0xBD]
            for page in family.userPages { XCTAssertNoThrow(try NTAGPermissions(family:family,pages:pages).requireWritable(page,authenticated:false)) }
            pages[2]?[2] |= 0x10
            XCTAssertThrowsError(try NTAGPermissions(family:family,pages:pages).requireWritable(4,authenticated:false))
            pages[2] = [0,0,0,0]
            pages[family.dynamicLockPage]?[0] = 2
            let locked = family == .ntag213 ? 18..<20 : 32..<48
            for page in locked { XCTAssertThrowsError(try NTAGPermissions(family:family,pages:pages).requireWritable(page,authenticated:false)) }
            XCTAssertNoThrow(try NTAGPermissions(family:family,pages:pages).requireWritable(family == .ntag213 ? 20 : 31,authenticated:false))
            pages[family.dynamicLockPage]?[0] = 0
            pages[family.dynamicLockPage+1]?[3] = 20
            XCTAssertNoThrow(try NTAGPermissions(family:family,pages:pages).requireWritable(19,authenticated:false))
            XCTAssertThrowsError(try NTAGPermissions(family:family,pages:pages).requireWritable(20,authenticated:false))
            XCTAssertNoThrow(try NTAGPermissions(family:family,pages:pages).requireWritable(20,authenticated:true))
            for page in [0,1,2,3,family.dynamicLockPage,family.unitCount-1] {
                XCTAssertThrowsError(try NTAGPermissions(family:family,pages:pages).requireWritable(page,authenticated:true))
            }
            pages[3]?[2] = 255
            XCTAssertThrowsError(try NTAGPermissions(family:family,pages:pages).requireWritable(4,authenticated:true))
        }
        XCTAssertThrowsError(try NTAGAuthentication.password([1,2,3,4]).encoded(supported:false))
        XCTAssertEqual(try NTAGAuthentication.passwordAndPACK([1,2,3,4],[5,6]).encoded(supported:true),[2,1,2,3,4,5,6])
    }
    static func permissionPages(_ family: CardFamily) -> [Int:[UInt8]] {
        [2:[0,0,0,0],3:[0xE1,0x10,UInt8(family.userPages.count*4/8),0],
         family.dynamicLockPage:[0,0,0,0xBD],family.dynamicLockPage+1:[0,0,0,0xFF],family.dynamicLockPage+2:[0,0,0,0]]
    }
    func testNTAGNDEFPreservesKnownLockTLVsAndRejectsReservedOverlap() throws {
        for (family,descriptor) in [(CardFamily.ntag213,[UInt8(0xA0),0x0C,0x34]),(.ntag215,[0x88,0x08,0x66]),(.ntag216,[0xE8,0x0E,0x66])] {
            var info = NFCCardInfo(uid:[4,1,2,3,4,5,6],atr:[]); info.detectedFamily = family
            var snapshot = ClassicSnapshot(card:info,blocks:Self.permissionPages(family))
            let capacity = Int(snapshot.blocks[3]![2]) * 8
            var area: [UInt8] = [1,3] + descriptor + [3,0,0xFE]
            area += Array(repeating:0,count:capacity-area.count)
            for (index,page) in family.userPages.enumerated() {
                snapshot.blocks[page] = index*4+4 <= area.count ? Array(area[index*4..<index*4+4]) : [0,0,0,0]
            }
            let ndef = try ClassicNDEF(snapshot:snapshot)
            XCTAssertTrue(ndef.writable)
            let changed = try ndef.replacing(with:[.text("NTAG")])
            XCTAssertEqual(Array(changed.prefix(5)),[1,3]+descriptor)
            snapshot.blocks[4] = [2,3,0x40,4]
            snapshot.blocks[5] = [2,3,0,0xFE]
            XCTAssertThrowsError(try ClassicNDEF(snapshot:snapshot))
        }
    }
    func testRawReaderAllowlistsRejectManagementAndProtectedWrites() throws {
        for command: UInt16 in [2001,2010,4000,5000,3001] { XCTAssertThrowsError(try CoreS3Policy.chameleon(command,data:[])) }
        for block in [0,3,127,143,255] {
            XCTAssertThrowsError(try CoreS3Policy.chameleon(2009,data:[0x60,UInt8(block)]+Array(repeating:255,count:6)+Array(repeating:0,count:16)))
        }
        for bank in [UHFMemoryBank.reserved,.tid] {
            let data: [UInt8] = [0,0,0,0,UInt8(bank.rawValue),0,1,0,1,0,0]
            XCTAssertThrowsError(try CoreS3Policy.uhf(0x49,data:data))
        }
        XCTAssertThrowsError(try CoreS3Policy.uhf(0x07,data:[1]))
        XCTAssertThrowsError(try CoreS3Policy.uhf(0xB6,data:[0,0]))
    }
}
