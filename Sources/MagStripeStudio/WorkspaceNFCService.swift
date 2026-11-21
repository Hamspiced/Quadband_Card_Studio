// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import Foundation
import MagStripeCore

/// Routes PC/SC operations unchanged; the Chameleon shares one exclusive HF/LF connection.
@MainActor final class WorkspaceNFCService: NFCServicing {
    private let pcsc: any NFCServicing
    private let lf: LFModel
    private let hub: CoreS3Hub?
    init(lf: LFModel, hub: CoreS3Hub? = nil, pcsc: any NFCServicing = NFCService()) { self.lf = lf; self.hub = hub; self.pcsc = pcsc }
    private func bridge<T>(_ reader: String, _ work: (CoreS3HFClient) async throws -> T) async throws -> T {
        guard let hub else { throw StripeError("CoreS3 is disconnected.") }
        return try await hub.withHF(reader,work)
    }
    private func isChameleon(_ reader: String) -> Bool { reader.hasPrefix("Chameleon Ultra · ") }
    func formatNDEF(reader: String, expected: ClassicSnapshot, overwrite: Bool) async throws -> ClassicSnapshot {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.formatNDEF(expected:expected,overwrite:overwrite) } }
        guard !isChameleon(reader) else { throw StripeError("Chameleon canonical formatting is not implemented.") }
        return try await pcsc.formatNDEF(reader:reader,expected:expected,overwrite:overwrite)
    }
    func readers() async throws -> [String] {
        var result: [String]
        do { result = try await pcsc.readers() } catch { if lf.hfReaderName == nil && hub?.hfReaderName == nil { throw error }; result = [] }
        if let name = hub?.hfReaderName { result.insert(name,at:0) }
        if let name = lf.hfReaderName { result.insert(name,at:0) }; return result
    }
    private func use<T>(_ reader: String, work: @escaping @MainActor (ChameleonHFClient) async throws -> T) async throws -> T {
        guard reader == lf.hfReaderName else { throw StripeError("Reconnect the Chameleon in LF Workspace, then refresh HF readers.") }
        return try await lf.withHF(work)
    }
    func identify(reader: String) async throws -> NFCCardInfo {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.identify() } }
        if isChameleon(reader) { return try await use(reader) { try await $0.identify() } }
        return try await pcsc.identify(reader:reader)
    }
    func read(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) async throws -> ClassicSnapshot {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.read(key:key,type:type,sector:sector,automatic:false) } }
        if isChameleon(reader) { return try await use(reader) { try await $0.read(key:key,type:type,sector:sector,automatic:false) } }
        return try await pcsc.read(reader:reader,key:key,type:type,sector:sector)
    }
    func readAutomatic(reader: String, key: [UInt8], type: ClassicKeyType, sector: Int?) async throws -> ClassicSnapshot {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.read(key:key,type:type,sector:sector) } }
        if isChameleon(reader) { return try await use(reader) { try await $0.read(key:key,type:type,sector:sector) } }
        return try await pcsc.readAutomatic(reader:reader,key:key,type:type,sector:sector)
    }
    func write(reader: String, block: Int, data: [UInt8], key: [UInt8], type: ClassicKeyType, expected: NFCCardInfo, previous: [UInt8]) async throws -> [UInt8] {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.write(block:block,data:data,key:key,type:type,expected:expected,previous:previous) } }
        if isChameleon(reader) { return try await use(reader) { try await $0.write(block:block,data:data,key:key,type:type,expected:expected,previous:previous) } }
        return try await pcsc.write(reader:reader,block:block,data:data,key:key,type:type,expected:expected,previous:previous)
    }
    func writeNDEF(reader: String, records: [NDEFRecord], expected: ClassicNDEF, key: [UInt8], type: ClassicKeyType) async throws -> ClassicSnapshot {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.writeNDEF(records,expected:expected,key:key,type:type) } }
        if isChameleon(reader) { return try await use(reader) { try await $0.writeNDEF(records,expected:expected,key:key,type:type) } }
        return try await pcsc.writeNDEF(reader:reader,records:records,expected:expected,key:key,type:type)
    }
    func writeProfile(reader: String, profile: CardCredentials, key: [UInt8], type: ClassicKeyType) async throws -> NFCCardInfo {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.writeProfile(profile,key:key,type:type) } }
        if isChameleon(reader) { return try await use(reader) { try await $0.writeProfile(profile,key:key,type:type) } }
        return try await pcsc.writeProfile(reader:reader,profile:profile,key:key,type:type)
    }
    func isPresent(reader: String) async throws -> Bool {
        if reader.hasPrefix("CoreS3 · ") { return try await bridge(reader) { try await $0.isPresent() } }
        if isChameleon(reader) { return try await use(reader) { try await $0.isPresent() } }
        return try await pcsc.isPresent(reader:reader)
    }
}
