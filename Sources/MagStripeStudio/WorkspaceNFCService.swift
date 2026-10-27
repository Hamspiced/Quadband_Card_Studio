// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import MagStripeCore

/// Routes PC/SC operations unchanged; the Chameleon shares one exclusive HF/LF connection.
@MainActor final class WorkspaceNFCService: NFCServicing {
    private let pcsc: any NFCServicing
    private let lf: LFModel
    init(lf: LFModel, pcsc: any NFCServicing = NFCService()) { self.lf = lf; self.pcsc = pcsc }
    private func isChameleon(_ reader: String) -> Bool { reader.hasPrefix("Chameleon Ultra · ") }
    func readers() async throws -> [String] {
        var result: [String]
        do { result = try await pcsc.readers() } catch { if lf.hfReaderName == nil { throw error }; result = [] }
        if let name = lf.hfReaderName { result.insert(name,at:0) }; return result
    }
    private func use<T>(_ reader: String, work: @escaping @MainActor (ChameleonHFClient) async throws -> T) async throws -> T {
        guard reader == lf.hfReaderName else { throw StripeError("Reconnect the Chameleon in LF Workspace, then refresh HF readers.") }
        return try await lf.withHF(work)
    }
    func identify(reader: String) async throws -> NFCCardInfo {
        if isChameleon(reader) { return try await use(reader) { try await $0.identify() } }
        return try await pcsc.identify(reader:reader)
    }
    func read(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) async throws -> ClassicSnapshot {
        if isChameleon(reader) { return try await use(reader) { try await $0.read(key:key,type:type,sector:sector,automatic:false) } }
        return try await pcsc.read(reader:reader,key:key,type:type,sector:sector)
    }
    func readAutomatic(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) async throws -> ClassicSnapshot {
        if isChameleon(reader) { return try await use(reader) { try await $0.read(key:key,type:type,sector:sector) } }
        return try await pcsc.readAutomatic(reader:reader,key:key,type:type,sector:sector)
    }
    func write(reader: String, block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) async throws -> [UInt8] {
        if isChameleon(reader) { return try await use(reader) { try await $0.write(block:block,data:data,key:key,type:type,expected:expected,previous:previous) } }
        return try await pcsc.write(reader:reader,block:block,data:data,key:key,type:type,expected:expected,previous:previous)
    }
    func writeNDEF(reader: String, records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) async throws -> ClassicSnapshot {
        if isChameleon(reader) { return try await use(reader) { try await $0.writeNDEF(records,expected:expected,key:key,type:type) } }
        return try await pcsc.writeNDEF(reader:reader,records:records,expected:expected,key:key,type:type)
    }
    func writeProfile(reader: String, profile: CardCredentials, key: [UInt8], type: ClassicKeyType) async throws -> NFCCardInfo {
        if isChameleon(reader) { return try await use(reader) { try await $0.writeProfile(profile,key:key,type:type) } }
        return try await pcsc.writeProfile(reader:reader,profile:profile,key:key,type:type)
    }
    func isPresent(reader: String) async throws -> Bool {
        if isChameleon(reader) { return try await use(reader) { try await $0.isPresent() } }
        return try await pcsc.isPresent(reader:reader)
    }
}
