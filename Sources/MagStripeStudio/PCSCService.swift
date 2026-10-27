// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import CPCSC
import MagStripeCore

private func checkPCSC(_ result: Int32) throws {
    guard result != 0 else { return }
    let message: String
    switch UInt32(bitPattern: result) {
    case 0x8010000C: message = "Place one NFC card on the reader and try again."
    case 0x80100069, 0x80100068: message = "The NFC card was removed or reset. Keep it on the reader and read it again."
    case 0x8010000B: message = "The NFC reader is busy. Close other NFC applications and try again."
    case 0x80100017, 0x80100009: message = "The NFC reader disconnected. Reconnect it and refresh the readers."
    case 0x8010001D, 0x8010001E: message = "macOS smart-card service is unavailable. Reconnect the reader and check its ACS macOS driver."
    case 0x8010002E: message = "No PC/SC NFC readers found. Connect your ACS reader and refresh."
    default: message = "NFC communication failed. Check the reader and card placement."
    }
    throw StripeError("\(message) (PC/SC \(String(format: "%08X", UInt32(bitPattern: result))))")
}

private final class PCSCContext {
    var handle: Int32 = 0
    init() throws { try checkPCSC(QBCreateContext(&handle)) }
    deinit { if handle != 0 { QBReleaseContext(handle) } }
    func readers() throws -> [String] {
        var length: UInt32 = 0
        let result = QBListReaders(handle, nil, &length)
        if UInt32(bitPattern: result) == 0x8010002E { return [] }
        try checkPCSC(result)
        guard length > 1, length < 65536 else { return [] }
        var buffer = [CChar](repeating: 0, count: Int(length))
        try checkPCSC(QBListReaders(handle, &buffer, &length))
        return buffer.prefix(Int(length)).split(separator: 0).map { String(decoding: $0.map { UInt8(bitPattern: $0) }, as: UTF8.self) }
    }
}

private final class PCSCCard: NFCTransport {
    let context: PCSCContext
    var handle: Int32 = 0
    var activeProtocol: UInt32 = 0
    var atr: [UInt8] = []
    private var transaction = false
    init(context: PCSCContext, reader: String) throws {
        self.context = context
        try checkPCSC(QBConnect(context.handle, reader, &handle, &activeProtocol))
        do {
            try checkPCSC(QBBegin(handle)); transaction = true
            // macOS PC/SC retains this transaction across SCardReconnect.
            // Clear a halted/authenticated state left by a previous operation.
            try checkPCSC(QBReset(handle, &activeProtocol))
            var names = [CChar](repeating: 0, count: 4096)
            var nameLength = UInt32(names.count), state: UInt32 = 0
            var bytes = [UInt8](repeating: 0, count: 64), atrLength: UInt32 = 64
            try checkPCSC(QBStatus(handle, &names, &nameLength, &state, &activeProtocol, &bytes, &atrLength))
            guard atrLength <= bytes.count else { throw StripeError("NFC card identification was too long.") }
            atr = Array(bytes.prefix(Int(atrLength)))
        } catch { close(); throw error }
    }
    private func close() {
        if transaction { QBEnd(handle); transaction = false }
        if handle != 0 { QBDisconnect(handle); handle = 0 }
    }
    deinit { close() }
    func resetAuthentication() throws {
        try checkPCSC(QBReset(handle, &activeProtocol))
    }
    func transmit(_ bytes: [UInt8]) throws -> [UInt8] {
        var response = [UInt8](repeating: 0, count: 258), length: UInt32 = 258
        try checkPCSC(QBTransmit(handle, activeProtocol, bytes, UInt32(bytes.count), &response, &length))
        guard length <= response.count else { throw StripeError("NFC response exceeded the receive buffer.") }
        return Array(response.prefix(Int(length)))
    }
}

// Blocking PC/SC calls run on this actor's executor, never the UI's main actor.
protocol NFCServicing: Sendable {
    func readers() async throws -> [String]
    func identify(reader: String) async throws -> NFCCardInfo
    func read(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) async throws -> ClassicSnapshot
    func write(reader: String, block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) async throws -> [UInt8]
    func readAutomatic(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) async throws -> ClassicSnapshot
    func writeNDEF(reader: String, records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) async throws -> ClassicSnapshot
    func isPresent(reader: String) async throws -> Bool
    func writeProfile(reader: String, profile: CardCredentials, key: [UInt8], type: ClassicKeyType) async throws -> NFCCardInfo
}

actor NFCService: NFCServicing {
    func readAutomatic(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) throws -> ClassicSnapshot {
        let card = try PCSCCard(context: PCSCContext(), reader: reader)
        return try ClassicClient(transport: card).readAutomatic(key: key, type: type, sector: sector)
    }
    func writeNDEF(reader: String, records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) throws -> ClassicSnapshot {
        let card = try PCSCCard(context: PCSCContext(), reader: reader)
        return try ClassicClient(transport: card).writeNDEF(records, expected: expected, key: key, type: type)
    }
    func isPresent(reader: String) throws -> Bool {
        let context = try PCSCContext()
        var present: Int32 = 0
        try checkPCSC(QBCardPresent(context.handle, reader, &present))
        return present != 0
    }
    func writeProfile(reader: String, profile: CardCredentials, key: [UInt8], type: ClassicKeyType) throws -> NFCCardInfo {
        let card = try PCSCCard(context: PCSCContext(), reader: reader)
        return try ClassicClient(transport: card).writeProfile(profile, key: key, type: type)
    }
    func readers() throws -> [String] { try PCSCContext().readers() }
    func identify(reader: String) throws -> NFCCardInfo {
        let card = try PCSCCard(context: PCSCContext(), reader: reader)
        return try ClassicClient(transport: card).identify()
    }
    func read(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) throws -> ClassicSnapshot {
        let card = try PCSCCard(context: PCSCContext(), reader: reader)
        return try ClassicClient(transport: card).read(key: key, type: type, sector: sector)
    }
    func write(reader: String, block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType,
               expected: NFCCardInfo, previous: [UInt8]) throws -> [UInt8] {
        let card = try PCSCCard(context: PCSCContext(), reader: reader)
        return try ClassicClient(transport: card).write(block: block, data: data, key: key, type: type, expectedCard: expected, previous: previous)
    }
}

// Reader configuration uses escape control; ACR122U also supports pseudo-APDUs with a card present.
actor PCSCReaderControl: ReaderControlTransport {
    let reader: String
    init(reader: String) { self.reader = reader }
    func control(_ command: [UInt8]) throws -> [UInt8] {
        guard ReaderFamily.detect(reader) != nil else { throw StripeError("Select an ACR122U or ACR1552U contactless reader.") }
        let context = try PCSCContext()
        guard try context.readers().contains(reader) else { throw StripeError("The selected reader is disconnected. Reconnect it and read settings again.") }
        let family = ReaderFamily.detect(reader)!
        var present: Int32 = 0
        let hasCard = QBCardPresent(context.handle,reader,&present) == 0 && present != 0
        var handle: Int32 = 0, activeProtocol: UInt32 = 0
        let connection = hasCard ? QBConnect(context.handle,reader,&handle,&activeProtocol) : QBConnectDirect(context.handle,reader,&handle)
        guard connection == 0 else {
            throw StripeError("Reader control connection failed (PC/SC \(String(format:"%08X",UInt32(bitPattern:connection)))). Place a card on the reader and try again; some Mac drivers require a card for reader controls. If that fails, check the ACS driver and close other reader apps.")
        }
        defer { QBDisconnect(handle) }
        var transaction = false
        if hasCard { try checkPCSC(QBBegin(handle)); transaction = true }
        defer { if transaction { QBEnd(handle) } }
        // Configuration must not reset the card/RF state between reader commands.
        // ACR122U API §6 permits the same commands via pseudo-APDU when a card is present.
        var reply = [UInt8](repeating:0,count:4096), count: UInt32 = 4096
        let result = family == .acr122 && hasCard
            ? QBTransmit(handle,activeProtocol,command,UInt32(command.count),&reply,&count)
            : QBControl(handle,command,UInt32(command.count),&reply,&count)
        guard result == 0 else { throw StripeError("Reader control failed (PC/SC \(String(format:"%08X",UInt32(bitPattern:result)))). Close other reader apps. This command may be unavailable on the reader firmware or its macOS driver.") }
        guard count <= reply.count else { throw StripeError("Reader control returned too much data.") }
        return Array(reply.prefix(Int(count)))
    }
}

protocol ReaderConfigServicing: Sendable {
    func read(reader: String) async throws -> ReaderSettingsSnapshot
    func apply(snapshot: ReaderSettingsSnapshot, edits: [String:Int]) async throws -> [ReaderApplyResult]
}
struct ReaderConfigService: ReaderConfigServicing {
    func read(reader: String) async throws -> ReaderSettingsSnapshot {
        try await ReaderConfigurationClient(transport:PCSCReaderControl(reader:reader)).read(reader:reader)
    }
    func apply(snapshot: ReaderSettingsSnapshot, edits: [String:Int]) async throws -> [ReaderApplyResult] {
        try await ReaderConfigurationClient(transport:PCSCReaderControl(reader:snapshot.reader)).apply(snapshot:snapshot,edits:edits)
    }
}
