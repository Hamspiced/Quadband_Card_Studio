import XCTest
import AppKit
import SwiftUI
import MagStripeCore
@testable import MagStripeStudio

final class CoreS3AppTests: XCTestCase {
    @MainActor func testInventoryLeaseRejectsCompetingReadersWithoutDisconnectingThem() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let model = AppModel(scanDevices:false); model.cores3.attach(session)
        model.uhf.useCoreS3 = true; model.uhf.connect()
        for _ in 0..<100 where model.uhf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertTrue(model.uhf.connected)
        model.lf.link = .cores3; model.lf.connect()
        for _ in 0..<100 where model.lf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertTrue(model.lf.connected)
        model.uhf.scan()
        for _ in 0..<100 where model.uhf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertTrue(model.uhf.scanning)
        let grove = try XCTUnwrap(model.cores3.hfReaderName), usb = try XCTUnwrap(model.lf.hfReaderName)
        do { _ = try await model.cores3.withHF(grove) { try await $0.identify() }; XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("Stop scanning")) }
        do { _ = try await model.nfc.service.identify(reader:usb); XCTFail() }
        catch { XCTAssertTrue(error.localizedDescription.contains("Stop scanning")) }
        try await Task.sleep(nanoseconds:250_000_000)
        XCTAssertTrue(model.uhf.connected); XCTAssertTrue(model.uhf.scanning); XCTAssertTrue(model.lf.connected)
        XCTAssertGreaterThan(device.requests.filter { $0.channel == 2 && $0.payload.count > 2 && $0.payload[2] == 0x22 }.count,1)
        model.uhf.stop()
        for _ in 0..<300 where model.uhf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertFalse(model.uhf.scanning); XCTAssertTrue(model.uhf.connected)
        let lease = try session.acquire(1); session.release(lease)
        let card = try await model.nfc.service.identify(reader:usb); XCTAssertEqual(card.family,.classic4k)
        model.cores3.disconnect()
        for _ in 0..<100 where model.cores3.connecting { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertEqual(device.mode,0); XCTAssertNil(model.cores3.session)
    }
    @MainActor func testBusyChameleonConnectCanBeRetriedAfterInventoryStops() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let model = AppModel(scanDevices:false); model.cores3.attach(session)
        model.uhf.useCoreS3 = true; model.uhf.connect()
        for _ in 0..<100 where model.uhf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        model.uhf.scan()
        for _ in 0..<100 where model.uhf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        model.lf.link = .cores3; model.lf.connect()
        for _ in 0..<100 where model.lf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertFalse(model.lf.connected); XCTAssertTrue(model.uhf.scanning)
        model.uhf.stop()
        for _ in 0..<300 where model.uhf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        model.lf.connect()
        for _ in 0..<100 where model.lf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertTrue(model.lf.connected); XCTAssertTrue(model.uhf.connected)
        model.cores3.disconnect()
        for _ in 0..<100 where model.cores3.connecting { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertNil(model.cores3.session)
    }
    @MainActor func testWorkspaceUSBChameleonRoutingAndNormalHubReleaseRestoreMode() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let model = AppModel(scanDevices:false); model.cores3.attach(session)
        model.lf.link = .cores3; model.lf.connect()
        for _ in 0..<100 where model.lf.busy { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertTrue(model.lf.connected)
        let name = try XCTUnwrap(model.lf.hfReaderName)
        let card = try await model.nfc.service.identify(reader:name)
        XCTAssertEqual(card.family,.classic4k); XCTAssertEqual(device.mode,1)
        model.cores3.disconnect()
        for _ in 0..<100 where model.cores3.connecting { try await Task.sleep(nanoseconds:1_000_000) }
        XCTAssertEqual(device.mode,0); XCTAssertFalse(model.lf.connected); XCTAssertNil(model.cores3.session)
        XCTAssertTrue(device.requests.dropFirst().allSatisfy { $0.channel == 3 })
    }
    @MainActor func testExistingM100ClientUsesBridgeAndKeepsInventorySemantics() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let transport = CoreS3M100Transport(session:session,generation:1), client = M100Client(transport:transport)
        let version = try await client.identify(); XCTAssertTrue(version.contains("JRD4035"))
        let settings = try await client.settings(); XCTAssertEqual(settings.power,20)
        var tags: [UHFTag] = []; transport.onTag = { tags.append($0) }
        _ = try await transport.exchange(0x22,data:[],timeout:1)
        XCTAssertEqual(tags.count,1)
        try device.inventoryEvent()
        XCTAssertEqual(tags.count,2)
        device.noTag = true
        _ = try await transport.exchange(0x22,data:[],timeout:1)
        XCTAssertEqual(tags.count,2)
        XCTAssertTrue(device.requests.dropFirst().allSatisfy { $0.channel == 2 && $0.operation == 32 })
        transport.close()
    }
    @MainActor func testBridgedChameleonUsesExistingClientModeAndExclusiveHFLFLease() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let transport = CoreS3ChameleonTransport(session:session,generation:1), client = ChameleonClient(transport:transport)
        try transport.begin()
        let info = try await client.identify()
        XCTAssertTrue(info.capabilities.contains(2009)); XCTAssertTrue(info.formats.contains(.em410x))
        XCTAssertThrowsError(try session.acquire(1))
        let card = try await client.scan(format:.em410x)
        XCTAssertEqual(card?.identifier,"0102030405"); XCTAssertEqual(device.mode,1)
        try await client.restoreMode(); XCTAssertEqual(device.mode,0)
        transport.end()
        let lease = try session.acquire(1); session.release(lease)
        do { _ = try await transport.exchange(2001,data:[],timeout:1); XCTFail() } catch {}
        XCTAssertFalse(device.chameleonCommands.contains(2001))
    }
    @MainActor func testMalformedInnerFramesCloseAffectedEndpoint() async throws {
        for channel: UInt8 in [2,3] {
            let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
            device.malformedInner = true
            var failed = false
            if channel == 2 {
                let transport = CoreS3M100Transport(session:session,generation:1); transport.onDisconnect = { _ in failed = true }
                do { _ = try await transport.exchange(0x03,data:[0],timeout:1); XCTFail() } catch {}
                let count = device.requests.count
                do { _ = try await transport.exchange(0x03,data:[0],timeout:1); XCTFail() } catch {}
                XCTAssertEqual(device.requests.count,count)
            } else {
                let transport = CoreS3ChameleonTransport(session:session,generation:1); transport.onDisconnect = { _ in failed = true }
                do { _ = try await transport.exchange(1000,data:[],timeout:1); XCTFail() } catch {}
                let count = device.requests.count
                do { _ = try await transport.exchange(1000,data:[],timeout:1); XCTFail() } catch {}
                XCTAssertEqual(device.requests.count,count)
            }
            XCTAssertTrue(failed)
        }
    }
    @MainActor func testGenerationsClearSelectionsCredentialsAndWritePreviews() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let model = AppModel(scanDevices:false); model.cores3.attach(session)
        model.nfc.selectedReader = try XCTUnwrap(model.cores3.hfReaderName)
        var card = NFCCardInfo(uid:[1,2,3,4],atr:[]); card.detectedFamily = .classic4k
        model.nfc.snapshot = ClassicSnapshot(card:card,blocks:[128:Array(repeating:0,count:16)])
        model.nfc.snapshotReader = model.nfc.selectedReader
        model.cores3.usePassword = true; model.cores3.password = "01020304"; model.cores3.pack = "1234"
        model.uhf.useCoreS3 = true; model.uhf.connected = true
        try device.changeGeneration()
        XCTAssertEqual(model.nfc.selectedReader,""); XCTAssertNil(model.nfc.snapshot)
        XCTAssertTrue(model.nfc.requiresExplicitReaderSelection)
        XCTAssertEqual(model.cores3.password,""); XCTAssertEqual(model.cores3.pack,""); XCTAssertFalse(model.cores3.usePassword)
        XCTAssertFalse(model.uhf.connected); XCTAssertNil(model.uhf.writePreview)
        XCTAssertEqual(model.cores3.session === session,true)
        XCTAssertNotEqual(model.cores3.hfReaderName,model.nfc.selectedReader)
        model.nfc.selectedReader = "Chameleon Ultra · CoreS3"; model.nfc.snapshot = ClassicSnapshot(card:card)
        try device.changeGeneration()
        XCTAssertEqual(model.nfc.selectedReader,""); XCTAssertNil(model.nfc.snapshot)
    }
    @MainActor func testPageAndLargeSectorEditorCapabilitiesAndLocks() async throws {
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let model = AppModel(scanDevices:false); model.cores3.attach(session)
        let hf = model.nfc
        hf.selectedReader = try XCTUnwrap(model.cores3.hfReaderName); hf.snapshotReader = hf.selectedReader
        hf.snapshot = Self.snapshot(.classic4k); hf.selectSector(32)
        XCTAssertEqual(hf.selectedBlock,128); XCTAssertEqual(hf.selectedUnits.count,16); XCTAssertEqual(hf.unitSize,16); XCTAssertTrue(hf.canWrite)
        hf.selectBlock(143); XCTAssertFalse(hf.canWrite)
        hf.snapshot = Self.snapshot(.ntag215); hf.selectSector(2); hf.selectBlock(32)
        XCTAssertEqual(hf.unitSize,4); XCTAssertTrue(hf.canWrite)
        hf.snapshot?.blocks[130]?[0] = 2
        XCTAssertFalse(hf.canWrite)
        hf.selectBlock(130); XCTAssertFalse(hf.canWrite)
        hf.snapshot = Self.snapshot(.ntag213); hf.selectedReader = "Chameleon Ultra · CoreS3"; hf.snapshotReader = hf.selectedReader; hf.selectBlock(4)
        XCTAssertFalse(hf.canWrite)
    }
    @MainActor func testRenderCoreS3PanelAndExpandedHFFamilies() async throws {
        _ = NSApplication.shared
        let device = AppBridgeFixture(), session = CoreS3Session(channel:device); _ = try await session.hello()
        let model = AppModel(scanDevices:false); model.cores3.attach(session)
        let directory = URL(fileURLWithPath:FileManager.default.currentDirectoryPath).appendingPathComponent(".build/ui-checks")
        try FileManager.default.createDirectory(at:directory,withIntermediateDirectories:true)
        try render(CoreS3ConnectionView(hub:model.cores3).padding(20).foregroundColor(.white).background(.black).environment(\.colorScheme,.dark),
                   size:NSSize(width:260,height:520),to:directory.appendingPathComponent("cores3-panel.png"))
        for family in [CardFamily.classic1k,.classic4k,.ntag213,.ntag215,.ntag216] {
            model.nfc.selectedReader = try XCTUnwrap(model.cores3.hfReaderName)
            model.nfc.readers = [model.nfc.selectedReader]
            model.nfc.snapshotReader = model.nfc.selectedReader
            model.nfc.snapshot = Self.snapshot(family)
            model.nfc.selectedSector = family == .classic4k ? 32 : 0
            model.nfc.selectBlock(family == .classic4k ? 128 : family.isNTAG ? 4 : 1)
            model.nfc.hfPage = .advanced
            try render(NFCView(model:model.nfc,hub:model.cores3),size:NSSize(width:1000,height:2400),
                       to:directory.appendingPathComponent("cores3-\(family.rawValue)-advanced.png"))
            model.nfc.profile = try CardCredentials(snapshot:Self.snapshot(family))
            model.nfc.profileOrigin = "\(family.rawValue).qcard"; model.nfc.hfPage = .tools; model.nfc.batchPresented = true
            try render(NFCView(model:model.nfc,hub:model.cores3),size:NSSize(width:1000,height:900),
                       to:directory.appendingPathComponent("cores3-\(family.rawValue)-profiles.png"))
            if family.isNTAG, let snapshot = model.nfc.snapshot {
                model.nfc.inspectRecords(snapshot); model.nfc.recordQueue = [try .text("First CoreS3 message")]
                XCTAssertNil(model.nfc.recordWriteError)
                for page in [NFCModel.HFPage.read,.write] {
                    model.nfc.hfPage = page
                    try render(NFCView(model:model.nfc,hub:model.cores3),size:NSSize(width:1000,height:1000),
                               to:directory.appendingPathComponent("cores3-\(family.rawValue)-records-\(page == .read ? "read" : "write").png"))
                }
            }
        }
    }
    @MainActor private func render<V:View>(_ view: V, size: NSSize, to url: URL) throws {
        let host = NSHostingView(rootView:view.environment(\.colorScheme,.dark)); host.frame = NSRect(origin:.zero,size:size)
        let window = NSWindow(contentRect:host.frame,styleMask:.borderless,backing:.buffered,defer:false)
        window.appearance = NSAppearance(named:.darkAqua)
        window.contentView = host; host.layoutSubtreeIfNeeded()
        RunLoop.current.run(until:Date().addingTimeInterval(0.05))
        host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in:host.bounds)); host.cacheDisplay(in:host.bounds,to:bitmap)
        let png = try XCTUnwrap(bitmap.representation(using:.png,properties:[:])); XCTAssertGreaterThan(png.count,1000)
        try png.write(to:url); window.contentView = nil
    }
    static func snapshot(_ family: CardFamily) -> ClassicSnapshot {
        var card = NFCCardInfo(uid:[1,2,3,4],atr:[]); card.detectedFamily = family
        var result = ClassicSnapshot(card:card,blocks:Dictionary(uniqueKeysWithValues:(0..<family.unitCount).map { ($0,Array(repeating:UInt8(0),count:family.unitSize)) }))
        if family.isClassic {
            for sector in 0..<family.sectorCount {
                result.blocks[family.blocks(in:sector).last!] = Array(repeating:255,count:6)+[0xFF,0x07,0x80,0x69]+Array(repeating:255,count:6)
                result.sectorKeys[sector] = ClassicCredential(bytes:Array(repeating:255,count:6),type:.a)
            }
        } else {
            result.blocks[3] = [0xE1,0x10,UInt8(family.userPages.count*4/8),0]
            result.blocks[family.dynamicLockPage+1] = [0,0,0,0xFF]
        }
        return result
    }
}

@MainActor private final class AppBridgeFixture: CoreS3ByteChannel {
    var onBytes: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?
    var requests: [CoreS3Frame] = []
    var chameleonCommands: [UInt16] = []
    var malformedInner = false, noTag = false
    var mode: UInt8 = 0
    var generation: UInt32 = 1
    private var nonce = ""
    private var decoder = CoreS3Decoder()
    func close() {}
    private func capabilities() throws -> [UInt8] {
        let object: [String:Any] = ["protocol":1,"deviceId":"app-fixture","name":"Quadband CoreS3","firmware":"0.1.0","clientNonce":nonce,
            "peripherals":[["channel":1,"generation":generation,"present":true,"kind":"rfid2","capabilities":["identify","classic1k","classic4k","ntag21x","ntag_auth"]],
                           ["channel":2,"generation":generation,"present":true,"kind":"jrd4035","capabilities":["m100"]],
                           ["channel":3,"generation":generation,"present":true,"kind":"chameleon","capabilities":["chameleon"]]]]
        return Array(try JSONSerialization.data(withJSONObject:object))
    }
    func changeGeneration() throws {
        generation += 1
        onBytes?(try CoreS3Frame(kind:2,session:42,requestID:0,channel:0,operation:128,payload:capabilities()).encoded())
    }
    private func inventory() throws -> [UInt8] {
        let body: [UInt8] = [0x10,0,1,2,3,4]
        let crc = M100Protocol.crc16(body)
        return try M100Frame(kind:2,command:0x22,data:[0xCF]+body+[UInt8(crc >> 8),UInt8(truncatingIfNeeded:crc)]).encoded()
    }
    func inventoryEvent() throws {
        onBytes?(try CoreS3Frame(kind:2,session:42,requestID:0,channel:2,operation:32,generation:generation,payload:inventory()).encoded())
    }
    func send(_ bytes: [UInt8]) throws {
        for request in try decoder.accept(bytes) {
            requests.append(request)
            let payload: [UInt8]
            if request.operation == 1 {
                nonce = request.payload.map { String(format:"%02x",$0) }.joined(); payload = try capabilities()
            } else if malformedInner { payload = [0,0,0] }
            else if request.channel == 2 {
                let command = request.payload[2]
                switch command {
                case 0x03: payload = try M100Frame(kind:1,command:command,data:[0]+Array("JRD4035 fixture".utf8)).encoded()
                case 0xB7: payload = try M100Frame(kind:1,command:command,data:[0x07,0xD0]).encoded()
                case 0x08: payload = try M100Frame(kind:1,command:command,data:[1]).encoded()
                case 0x22: payload = noTag ? try M100Frame(kind:1,command:0xFF,data:[0x15]).encoded() : try inventory()
                default: payload = try M100Frame(kind:1,command:command,data:[0]).encoded()
                }
            } else {
                let command = UInt16(request.payload[2])*256+UInt16(request.payload[3]); chameleonCommands.append(command)
                var status: UInt16 = 0x68, data: [UInt8] = []
                switch command {
                case 1000: data = [2,1]
                case 1001: mode = request.payload[9]
                case 1002: data = [mode]
                case 1033: data = [0]
                case 1035: data = [UInt16(1001),1002,2000,2008,2009,3000,3016].flatMap { [UInt8($0 >> 8),UInt8(truncatingIfNeeded:$0)] }
                case 3000: status = 0x40; data = [1,2,3,4,5]
                case 2000: status = 0; data = [4,1,2,3,4,2,0,0x18,0]
                default: break
                }
                payload = try ChameleonFrame(command:command,status:status,data:data).encoded()
            }
            onBytes?(try CoreS3Frame(kind:1,session:42,requestID:request.requestID,channel:request.channel,operation:request.operation,
                                    generation:request.generation,payload:payload).encoded())
        }
    }
}
