import XCTest
@testable import MagStripeCore

final class CoreS3WorkflowTests: XCTestCase {
    @MainActor func testCanonicalFormattingWireSnapshotsGatesAndEmptyResponse() async throws {
        for family in NDEFBulkJob.families {
            let device = BridgeCardFixture(family)
            device.memory = bulkSnapshot(family,blank:true).blocks
            let session = CoreS3Session(channel:device); _ = try await session.hello()
            let lease = try session.acquire(1), hf = CoreS3HFClient(session:session,lease:lease)
            let snapshot = try await hf.read(key:NDEFFormatting.factory,type:.a,automatic:false)
            do { _ = try await hf.formatNDEF(expected:snapshot); XCTFail() } catch {}
            XCTAssertFalse(device.requests.contains { $0.operation == 22 || $0.operation == 23 })
            session.release(lease); session.close()

            let enabled = BridgeCardFixture(family)
            enabled.memory = bulkSnapshot(family,blank:true).blocks; enabled.formattingCapabilities = true
            let enabledSession = CoreS3Session(channel:enabled); _ = try await enabledSession.hello()
            let enabledLease = try enabledSession.acquire(1), writer = CoreS3HFClient(session:enabledSession,lease:enabledLease)
            let expected = try await writer.read(key:NDEFFormatting.factory,type:.a,automatic:false)
            let result = try await writer.formatNDEF(expected:expected)
            XCTAssertTrue(try ClassicNDEF(snapshot:result).records.isEmpty)
            let request = try XCTUnwrap(enabled.requests.first { $0.operation == (family == .classic1k ? 22 : 23) })
            XCTAssertEqual(request.payload,[UInt8(expected.card.uid.count)]+expected.card.uid+[0]+(try NDEFFormatting.snapshotBytes(expected,overwrite:false)))
            XCTAssertEqual(request.generation,enabledLease.generation)
            XCTAssertEqual(enabled.requests.filter { $0.operation == request.operation }.count,1)
            XCTAssertFalse(enabled.requests.contains { $0.operation == 18 || $0.operation == 20 })
            enabledSession.release(enabledLease); enabledSession.close()
        }
    }
    @MainActor func testCanonicalTimeout120IsBoundedAndOnlyForFormatting() async throws {
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1)
        do { _ = try await session.perform(lease,operation:22,timeout:121); XCTFail() } catch {}
        do { _ = try await session.perform(lease,operation:16,timeout:120); XCTFail() } catch {}
        XCTAssertEqual(device.requests.count,1)
        // Unsupported operation response proves the 120-second request was admitted.
        do { _ = try await session.perform(lease,operation:22,timeout:120); XCTFail() }
        catch let error as CoreS3Error { XCTAssertEqual(error.status,12) }
        XCTAssertEqual(device.requests.count,2)
        session.close()
    }
    @MainActor func testModesBlockPeripheralWorkButKeepControlAndLegacyCompatibility() async throws {
        for mode in ["home","standalone","connected"] {
            let device = BridgeCardFixture(.classic1k)
            device.modeMetadata = ["mode":mode,"owner":mode == "home" ? "none" : mode == "connected" ? "desktop" : "local","modePending":false]
            let session = CoreS3Session(channel:device)
            let value = try await session.hello()
            XCTAssertEqual(value.mode,mode); XCTAssertEqual(value.remoteAllowed,mode == "connected")
            if mode == "connected" {
                let lease = try session.acquire(1); session.release(lease)
            } else {
                for channel: UInt8 in [1,2,3] { XCTAssertThrowsError(try session.acquire(channel)) }
            }
            let echo = try await session.ping([7]); XCTAssertEqual(echo,[7])
            _ = try await session.refreshCapabilities()
            XCTAssertTrue(device.requests.allSatisfy { $0.channel == 0 })
            session.close()
        }
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        let legacy = try await session.hello()
        XCTAssertNil(legacy.mode); XCTAssertTrue(legacy.remoteAllowed)
        session.close()
    }
    @MainActor func testModeTransitionPreservesInflightReplyWithoutReplayOrLeaseRevival() async throws {
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1)
        device.hold = true
        let operation = Task { try await session.perform(lease,operation:18,payload:[9],write:true) }
        for _ in 0..<100 where device.requests.count == 1 { await Task.yield() }
        let request = try XCTUnwrap(device.requests.last); XCTAssertEqual(request.operation,18)
        device.modeMetadata = ["mode":"connected","owner":"desktop","modePending":true]
        try device.modeEvent()
        XCTAssertThrowsError(try session.acquire(2)); XCTAssertFalse(device.closed)
        device.onBytes?(try CoreS3Frame(kind:1,session:99,requestID:request.requestID,channel:1,operation:18,
                                       generation:request.generation,payload:[9]).encoded())
        let result = try await operation.value; XCTAssertEqual(result,[9]); XCTAssertFalse(device.closed)
        device.hold = false
        let echo = try await session.ping([8]); XCTAssertEqual(echo,[8])
        device.modeMetadata = ["mode":"connected","owner":"desktop","modePending":false]
        try device.modeEvent()
        do { _ = try await session.perform(lease,operation:18,payload:[9]); XCTFail() } catch {}
        XCTAssertEqual(device.requests.filter { $0.channel != 0 }.count,1)
        let replacement = try session.acquire(1); session.release(replacement)
        session.close()
    }
    @MainActor func testModeChangeInvalidatesIdleLease() async throws {
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1)
        device.modeMetadata = ["mode":"standalone","owner":"local","modePending":false]
        try device.modeEvent()
        _ = try await session.refreshCapabilities()
        do { _ = try await session.perform(lease,operation:16); XCTFail() } catch {}
        XCTAssertEqual(device.requests.count,2); XCTAssertFalse(device.closed)
        session.close()
    }
    @MainActor func testFirstNDEFMessageOnKnownAllNULLNTAGPreservesProtectedMetadata() async throws {
        for family in [CardFamily.ntag213,.ntag215,.ntag216] {
            let device = BridgeCardFixture(family); device.memory[4] = [0,0,0,0]
            let session = CoreS3Session(channel:device); _ = try await session.hello()
            let lease = try session.acquire(1), hf = CoreS3HFClient(session:session,lease:lease)
            let snapshot = try await hf.read(key:[],type:.a), empty = try ClassicNDEF(snapshot:snapshot)
            XCTAssertTrue(empty.records.isEmpty); XCTAssertTrue(empty.writable)
            let protected = device.memory.filter { !family.isData($0.key) }
            let result = try await hf.writeNDEF([.text("First NDEF message")],expected:empty,key:[],type:.a)
            XCTAssertEqual(try ClassicNDEF(snapshot:result).records.first?.textValue,"First NDEF message")
            XCTAssertEqual(device.memory.filter { !family.isData($0.key) },protected)
            XCTAssertEqual(device.writes.first?.1[1],0)
            XCTAssertTrue(device.writes.allSatisfy { family.userPages.contains($0.0) })
            session.release(lease)
        }
    }
    @MainActor func testUnauthorizedHELLOFailsClosedWithoutCapabilitiesOrRetry() async {
        let device = BridgeCardFixture(.classic1k); device.helloStatus = 3
        let session = CoreS3Session(channel:device)
        do { _ = try await session.hello(); XCTFail() }
        catch let error as CoreS3Error { XCTAssertEqual(error.status,3) }
        catch { XCTFail(error.localizedDescription) }
        XCTAssertNil(session.capabilities); XCTAssertTrue(device.closed); XCTAssertEqual(device.requests.count,1)
    }
    @MainActor func testControlRefreshAndPingUseCurrentNonceAndMonotonicIDs() async throws {
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        _ = try await session.hello()
        let payload: [UInt8] = [0x51,0x42,0,0x7E]
        let echo = try await session.ping(payload); XCTAssertEqual(echo,payload)
        _ = try await session.refreshCapabilities()
        XCTAssertEqual(device.requests.map(\.requestID),[1,2,3])
        XCTAssertTrue(device.requests.dropFirst().allSatisfy { $0.session == 99 && $0.channel == 0 && $0.generation == 0 })
    }
    @MainActor func testPairingTimeoutIsBoundedConfigurableAndNeverRetried() async {
        let device = BridgeCardFixture(.classic1k); device.hold = true
        let session = CoreS3Session(channel:device)
        do { _ = try await session.hello(timeout:0.02); XCTFail() } catch {}
        XCTAssertEqual(device.requests.count,1); XCTAssertTrue(device.closed)
    }
    @MainActor func testHELLOAndCapabilitiesEventCanArriveInOneChunk() async throws {
        let device = BridgeCardFixture(.classic1k); device.helloBurst = true
        let session = CoreS3Session(channel:device)
        var updates = 0; session.onCapabilities = { _ in updates += 1 }
        _ = try await session.hello()
        XCTAssertEqual(updates,2); XCTAssertFalse(device.closed)
        let lease = try session.acquire(1); session.release(lease)
    }
    @MainActor func testMalformedCardHintsCannotBecomeWritableFamilies() async throws {
        for (family,override) in [(CardFamily.classic4k,["sak":8]),(.ntag213,["version":"0004040201001303"])] {
            let device = BridgeCardFixture(family); device.identityOverride = override
            let session = CoreS3Session(channel:device); _ = try await session.hello()
            let lease = try session.acquire(1), hf = CoreS3HFClient(session:session,lease:lease)
            do { _ = try await hf.identify(); XCTFail() } catch {}
            XCTAssertTrue(device.writes.isEmpty); session.release(lease)
        }
    }
    @MainActor func testBusySessionRejectsSecondRequestBeforeTransmission() async throws {
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1); device.hold = true
        let pending = Task { try await session.perform(lease,operation:16) }
        for _ in 0..<20 where device.requests.count == 1 { await Task.yield() }
        do { _ = try await session.perform(lease,operation:16); XCTFail() } catch {}
        XCTAssertEqual(device.requests.count,2)
        pending.cancel(); _ = try? await pending.value
    }
    @MainActor func testInterruptedNDEFNeverPublishesPartialMessageOrRetriesWrite() async throws {
        let device = BridgeCardFixture(.ntag213), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1)
        let hf = CoreS3HFClient(session:session,lease:lease), key = Array(repeating:UInt8(255),count:6)
        let snapshot = try await hf.read(key:key,type:.a), expected = try ClassicNDEF(snapshot:snapshot)
        device.rejectWriteNumber = 2
        do { _ = try await hf.writeNDEF([.text(String(repeating:"abc ",count:20))],expected:expected,key:key,type:.a); XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("unverified")) }
        XCTAssertEqual(device.writes.count,2); XCTAssertEqual(device.memory[4]?[1],0)
        session.release(lease)
    }
    @MainActor func testHELLORequiresFreshNonceAndExclusiveLease() async throws {
        let device = BridgeCardFixture(.classic4k), session = CoreS3Session(channel:device)
        let capabilities = try await session.hello()
        XCTAssertEqual(capabilities.peripherals.count,3)
        XCTAssertEqual(device.requests.first?.payload.count,8)
        let lease = try session.acquire(1)
        XCTAssertThrowsError(try session.acquire(2))
        session.release(lease)
        let second = try session.acquire(2); session.release(second)
        XCTAssertEqual(device.requests.map(\.requestID),[1])
        session.close()
        XCTAssertThrowsError(try session.acquire(1))
        let rejected = BridgeCardFixture(.classic1k); rejected.badNonce = true
        do { _ = try await CoreS3Session(channel:rejected).hello(); XCTFail() } catch {}
        XCTAssertTrue(rejected.closed)
    }
    @MainActor func testGenerationChangesInvalidateLeaseAndRejectStaleResults() async throws {
        let device = BridgeCardFixture(.classic4k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1)
        try device.changeGeneration()
        do { _ = try await session.perform(lease,operation:16); XCTFail() } catch {}
        XCTAssertEqual(device.requests.count,1)
        let next = try session.acquire(1)
        device.badResponseGeneration = true
        do { _ = try await session.perform(next,operation:16); XCTFail() } catch {}
        XCTAssertTrue(device.closed)
    }
    @MainActor func testWriteTimeoutAndCancellationNeverReplay() async throws {
        for cancel in [false,true] {
            let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
            _ = try await session.hello(); let lease = try session.acquire(1); device.hold = true
            let task = Task { try await session.perform(lease,operation:18,payload:[1],write:true,timeout:0.02) }
            for _ in 0..<10 where device.requests.count == 1 { await Task.yield() }
            if cancel { task.cancel() }
            do { _ = try await task.value; XCTFail() } catch { XCTAssertTrue(error.localizedDescription.contains("unverified")) }
            XCTAssertEqual(device.requests.filter { $0.operation == 18 }.count,1)
            XCTAssertTrue(device.closed)
        }
    }
    @MainActor func testNoCardOnlyMapsToAbsenceAndAuthErrorsAreNotRemoval() async throws {
        let device = BridgeCardFixture(.classic1k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1); defer { session.release(lease) }
        let hf = CoreS3HFClient(session:session,lease:lease)
        device.identifyStatus = 10
        let absent = try await hf.isPresent(); XCTAssertFalse(absent)
        device.identifyStatus = 14
        do { _ = try await hf.isPresent(); XCTFail() } catch let error as CoreS3Error { XCTAssertEqual(error.status,14) }
    }
    @MainActor func testClassic4KReadProfileAndProtectedSectors() async throws {
        let device = BridgeCardFixture(.classic4k), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1); defer { session.release(lease) }
        let hf = CoreS3HFClient(session:session,lease:lease), key = Array(repeating:UInt8(255),count:6)
        let snapshot = try await hf.read(key:key,type:.a)
        XCTAssertEqual(snapshot.blocks.count,256); XCTAssertEqual(snapshot.sectorKeys.count,40)
        for sector in [31,32,39] {
            let partial = try await hf.read(key:key,type:.a,sector:sector)
            XCTAssertEqual(Set(partial.blocks.keys),Set(CardFamily.classic4k.blocks(in:sector)))
        }
        var source = snapshot
        for unit in source.card.family.dataUnits { source.blocks[unit] = Array(repeating:UInt8(unit),count:16) }
        let profile = try CardCredentials(snapshot:source,name:"4K fixture")
        let protected = device.memory.filter { !CardFamily.classic4k.isData($0.key) }
        _ = try await hf.writeProfile(profile,key:key,type:.a)
        XCTAssertEqual(device.writes.count,215)
        XCTAssertEqual(device.memory.filter { !CardFamily.classic4k.isData($0.key) },protected)
        for unit in [0,3,127,143,255] {
            do { _ = try await hf.write(block:unit,data:Array(repeating:0,count:16),key:key,type:.a,expected:snapshot.card,previous:Array(repeating:0,count:16)); XCTFail() } catch {}
        }
        XCTAssertEqual(device.writes.count,215)
    }
    @MainActor func testProfilePermissionAndFamilyMismatchPreflightBeforeAnyWrite() async throws {
        for family in [CardFamily.classic1k,.classic4k,.ntag213,.ntag215,.ntag216] {
            let device = BridgeCardFixture(family), session = CoreS3Session(channel:device)
            _ = try await session.hello(); let lease = try session.acquire(1)
            let hf = CoreS3HFClient(session:session,lease:lease), key = Array(repeating:UInt8(255),count:6)
            let source = try await hf.read(key:key,type:.a), profile = try CardCredentials(snapshot:source)
            if family.isClassic { device.memory[family.unitCount-1] = Array(repeating:0,count:16) }
            else { device.memory[family.dynamicLockPage]?[0] = 1 }
            do { _ = try await hf.writeProfile(profile,key:key,type:.a); XCTFail() } catch {}
            XCTAssertTrue(device.writes.isEmpty)
            session.release(lease)
        }
        let source = BridgeCardFixture(.classic4k), sourceSession = CoreS3Session(channel:source)
        _ = try await sourceSession.hello(); let sourceLease = try sourceSession.acquire(1)
        let profile = try CardCredentials(snapshot:await CoreS3HFClient(session:sourceSession,lease:sourceLease).read(key:Array(repeating:255,count:6),type:.a))
        let target = BridgeCardFixture(.classic1k), targetSession = CoreS3Session(channel:target)
        _ = try await targetSession.hello(); let targetLease = try targetSession.acquire(1)
        do { _ = try await CoreS3HFClient(session:targetSession,lease:targetLease).writeProfile(profile,key:Array(repeating:255,count:6),type:.a); XCTFail() } catch {}
        XCTAssertTrue(target.writes.isEmpty)
    }
    @MainActor func testNTAGTypedPagesProfileAndLengthLastNDEFForEveryFamily() async throws {
        for family in [CardFamily.ntag213,.ntag215,.ntag216] {
            let device = BridgeCardFixture(family), session = CoreS3Session(channel:device)
            _ = try await session.hello(); let lease = try session.acquire(1); defer { session.release(lease) }
            let hf = CoreS3HFClient(session:session,lease:lease), key = Array(repeating:UInt8(255),count:6)
            let snapshot = try await hf.read(key:key,type:.a)
            XCTAssertNil(snapshot.blocks[family.unitCount-1]); XCTAssertNil(snapshot.blocks[family.unitCount-2])
            let before = device.memory.filter { !family.isData($0.key) }
            let expected = try ClassicNDEF(snapshot:snapshot), records = [try NDEFRecord.text("CoreS3 NTAG test")]
            let result = try await hf.writeNDEF(records,expected:expected,key:key,type:.a)
            XCTAssertEqual(try ClassicNDEF(snapshot:result).records,records)
            XCTAssertEqual(device.writes.first?.1[1],0)
            XCTAssertEqual(device.memory.filter { !family.isData($0.key) },before)
            let profile = try CardCredentials(snapshot:result)
            XCTAssertEqual(profile.blocks.count,family.userPages.count)
            _ = try await hf.writeProfile(profile,key:key,type:.a)
            XCTAssertTrue(device.writes.allSatisfy { family.userPages.contains($0.0) })
            do { _ = try await hf.readPages(family.unitCount-1,count:2,card:result.card); XCTFail() } catch {}
        }
    }
    @MainActor func testNTAGCredentialIsExplicitPACKValidatedAndNeverSerialized() async throws {
        let device = BridgeCardFixture(.ntag213), session = CoreS3Session(channel:device)
        _ = try await session.hello(); let lease = try session.acquire(1); defer { session.release(lease) }
        device.requirePassword = true
        let key = Array(repeating:UInt8(255),count:6)
        for auth in [NTAGAuthentication.none,.password([9,9,9,9]),.passwordAndPACK([1,2,3,4],[0,0])] {
            do { _ = try await CoreS3HFClient(session:session,lease:lease,authentication:auth).read(key:key,type:.a); XCTFail() }
            catch let error as CoreS3Error { XCTAssertEqual(error.status,11) }
        }
        let hf = CoreS3HFClient(session:session,lease:lease,authentication:.passwordAndPACK([1,2,3,4],[0x12,0x34]))
        let snapshot = try await hf.read(key:key,type:.a), profile = try CardCredentials(snapshot:snapshot)
        let saved = String(decoding:try profile.encoded(),as:UTF8.self)
        XCTAssertFalse(saved.contains("password")); XCTAssertFalse(saved.contains("PACK")); XCTAssertFalse(saved.contains("uid"))
        _ = try await hf.write(block:20,data:[1,1,1,1],key:key,type:.a,expected:snapshot.card,previous:snapshot.blocks[20]!)
        XCTAssertEqual(device.writes.count,1)
    }
    @MainActor func testMAD2CRCAndLargeSectorAllocationPreserveNonNDEFData() async throws {
        let device = BridgeCardFixture(.classic4k); try device.installMAD2()
        let session = CoreS3Session(channel:device); _ = try await session.hello()
        let lease = try session.acquire(1), hf = CoreS3HFClient(session:session,lease:lease), key = Array(repeating:UInt8(255),count:6)
        let snapshot = try await hf.read(key:key,type:.a), expected = try ClassicNDEF(snapshot:snapshot)
        XCTAssertEqual(expected.blocks,Array(4...6)+Array(128...142)+Array(240...254))
        let untouched = device.memory.filter { !expected.blocks.contains($0.key) }
        let result = try await hf.writeNDEF([.text(String(repeating:"data ",count:65))],expected:expected,key:key,type:.a)
        XCTAssertEqual(try ClassicNDEF(snapshot:result).records.count,1)
        XCTAssertEqual(device.memory.filter { !expected.blocks.contains($0.key) },untouched)
        device.memory[64]?[0] ^= 1
        let broken = try await hf.read(key:key,type:.a)
        XCTAssertThrowsError(try ClassicNDEF(snapshot:broken))
        session.release(lease)
    }
}

@MainActor final class BridgeCardFixture: CoreS3ByteChannel {
    var onBytes: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    let family: CardFamily
    var uid: [UInt8]
    var memory: [Int:[UInt8]]
    var requests: [CoreS3Frame] = []
    var writes: [(Int,[UInt8])] = []
    var hold = false, closed = false, badNonce = false, badResponseGeneration = false, requirePassword = false
    var identifyStatus: UInt16 = 0
    var helloStatus: UInt16 = 0
    var identityOverride: [String:Any] = [:]
    var modeMetadata: [String:Any] = [:]
    var helloBurst = false
    var formattingCapabilities = false
    var rejectWriteNumber: Int?
    var generation: UInt32 = 1
    var clientNonce = ""
    private var decoder = CoreS3Decoder()
    init(_ family: CardFamily) {
        self.family = family; uid = family.isClassic ? [1,2,3,4] : [4,1,2,3,4,5,6]
        memory = Dictionary(uniqueKeysWithValues:(0..<family.unitCount).map { ($0,Array(repeating:UInt8(0),count:family.unitSize)) })
        if family.isClassic {
            for sector in 0..<family.sectorCount { memory[family.blocks(in:sector).last!] = Array(repeating:255,count:6) + [0xFF,0x07,0x80,0x69] + Array(repeating:255,count:6) }
        } else {
            for (page,bytes) in CoreS3ProtocolTests.permissionPages(family) { memory[page] = bytes }
            memory[4] = [3,0,0xFE,0]
        }
    }
    func capabilities() throws -> [UInt8] {
        var object: [String:Any] = ["protocol":1,"deviceId":"simulated-core","name":"Quadband CoreS3","firmware":"0.1.0","clientNonce":clientNonce,
            "peripherals":[
                ["channel":1,"generation":generation,"present":true,"kind":"rfid2","capabilities":["identify",family.isClassic ? family.rawValue : "ntag21x","ntag_auth"] + (formattingCapabilities ? ["classic1k_format","ntag_initialize"] : [])],
                ["channel":2,"generation":1,"present":true,"kind":"jrd4035","capabilities":["m100"]],
                ["channel":3,"generation":1,"present":false,"kind":"chameleon","capabilities":[]]]]
        object.merge(modeMetadata) { _,new in new }
        return Array(try JSONSerialization.data(withJSONObject:object))
    }
    func changeGeneration() throws {
        generation += 1
        onBytes?(try CoreS3Frame(kind:2,session:99,requestID:0,channel:0,operation:128,payload:capabilities()).encoded())
    }
    func modeEvent() throws {
        onBytes?(try CoreS3Frame(kind:2,session:99,requestID:0,channel:0,operation:128,payload:capabilities()).encoded())
    }
    func close() { closed = true }
    func send(_ bytes: [UInt8]) throws {
        for request in try decoder.accept(bytes) {
            requests.append(request)
            if hold { continue }
            var payload: [UInt8] = [], status: UInt16 = 0
            if request.operation == 1 {
                clientNonce = badNonce ? "0000000000000000" : request.payload.map { String(format:"%02x",$0) }.joined()
                payload = helloStatus == 0 ? try capabilities() : []; status = helloStatus
            } else if request.operation == 2 { payload = try capabilities() }
            else if request.operation == 3 { payload = request.payload }
            else if request.generation != generation && request.channel == 1 { status = 6 }
            else if request.operation == 16 {
                status = identifyStatus
                let version: [UInt8] = [0,4,4,2,1,0,family == .ntag213 ? 0x0F : family == .ntag215 ? 0x11 : 0x13,3]
                var object: [String:Any] = ["uid":uid.map { String(format:"%02x",$0) }.joined(),
                    "atqa":family == .classic4k ? "0200" : family == .classic1k ? "0400" : "4400",
                    "sak":family == .classic4k ? 0x18 : family == .classic1k ? 8 : 0,"family":family.rawValue,
                    "blockCount":family.isClassic ? family.unitCount : 0,"pageCount":family.isNTAG ? family.unitCount : 0,
                    "version":family.isNTAG ? version.map { String(format:"%02x",$0) }.joined() : ""]
                for (key,value) in identityOverride { object[key] = value }
                payload = Array(try JSONSerialization.data(withJSONObject:object))
            } else if [UInt8(22),23].contains(request.operation), formattingCapabilities {
                let base = 1 + Int(request.payload[0])
                let count = family.isClassic ? 64 : family.unitCount-2
                let expected = (0..<count).flatMap { memory[$0]! }
                guard Array(request.payload[1..<base]) == uid, request.payload[base] <= 1,
                      Array(request.payload.dropFirst(base+1)) == expected else { throw StripeError("Invalid canonical bridge request snapshot.") }
                for (unit,desired) in try NDEFFormatting.canonicalUnits(family) { memory[unit] = desired }
            } else if [UInt8(17),18,19,20].contains(request.operation) {
                let base = 1 + Int(request.payload[0]), unit = Int(request.payload[base]) + Int(request.payload[base+1])*256
                guard Array(request.payload[1..<base]) == uid else { throw StripeError("Simulated UID mismatch.") }
                if request.operation == 17 { payload = memory[unit]! }
                if request.operation == 18 {
                    let at = base+9, previous = Array(request.payload[at..<at+16]), desired = Array(request.payload[at+16..<at+32])
                    if previous != memory[unit] { status = 17 } else { memory[unit] = desired; writes.append((unit,desired)); payload = desired }
                }
                if request.operation == 19 || request.operation == 20 {
                    let authIndex = base + (request.operation == 19 ? 3 : 2), mode = request.payload[authIndex]
                    let authSize = mode == 0 ? 1 : mode == 1 ? 5 : 7
                    if requirePassword {
                        let password = mode == 0 ? [] : Array(request.payload[authIndex+1..<authIndex+5])
                        if password != [1,2,3,4] || (mode == 2 && Array(request.payload[authIndex+5..<authIndex+7]) != [0x12,0x34]) { status = 11 }
                    }
                    if status == 0, request.operation == 19 {
                        let count = Int(request.payload[base+2]); payload = (unit..<unit+count).flatMap { memory[$0]! }
                    } else if status == 0 {
                        let at = authIndex+authSize, previous = Array(request.payload[at..<at+4]), desired = Array(request.payload[at+4..<at+8])
                        if previous != memory[unit] { status = 17 } else { memory[unit] = desired; writes.append((unit,desired)); payload = desired }
                    }
                }
            } else if request.operation != 21 { status = 12 }
            if [UInt8(18),20].contains(request.operation), writes.count == rejectWriteNumber { status = 15; payload = [] }
            let reply = try CoreS3Frame(kind:1,session:request.operation == 1 && helloStatus != 0 ? 0 : 99,requestID:request.requestID,channel:request.channel,operation:request.operation,
                status:status,generation:badResponseGeneration ? request.generation+1 : request.generation,payload:payload).encoded()
            if request.operation == 1 && helloBurst {
                onBytes?(try reply + CoreS3Frame(kind:2,session:99,requestID:0,channel:0,operation:128,payload:capabilities()).encoded())
                continue
            }
            for start in stride(from:0,to:reply.count,by:13) { onBytes?(Array(reply[start..<min(reply.count,start+13)])) }
        }
    }
    func installMAD2() throws {
        var mad = Array(repeating:UInt8(0),count:32); mad[1] = 1; mad[2] = 3; mad[3] = 0xE1
        mad[0] = ClassicNDEF.crc(Array(mad.dropFirst())); memory[1] = Array(mad.prefix(16)); memory[2] = Array(mad.suffix(16)); memory[3]?[9] = 0xC2
        var mad2 = Array(repeating:UInt8(0),count:48)
        for sector in [32,39] { mad2[2+(sector-17)*2] = 3; mad2[3+(sector-17)*2] = 0xE1 }
        mad2[0] = ClassicNDEF.crc(Array(mad2.dropFirst()))
        for index in 0..<3 { memory[64+index] = Array(mad2[index*16..<index*16+16]) }; memory[67]?[9] = 0
        memory[7]?[9] = 0x40; memory[143]?[9] = 0x40; memory[255]?[9] = 0x40; memory[4] = [3,0,0xFE]+Array(repeating:0,count:13)
    }
}
