import Foundation

public struct NDEFBulkJob {
    public static let families: [CardFamily] = [.classic1k,.ntag213,.ntag215,.ntag216]
    public let records: [NDEFRecord]
    public let family: CardFamily
    public let overwrite: Bool
    public let format: Bool
    public let encodedBytes: Int
    public var canonicalCapacity: Int {
        guard family != .classic1k else { return 716 }
        guard let cc = try? NDEFFormatting.cc(family) else { return 0 }
        let room = Int(cc[2])*8
        return max(min(254,room-2),room-4)
    }
    public init(records: [NDEFRecord], family: CardFamily, overwrite: Bool = false, format: Bool = false) throws {
        guard (1...3).contains(records.count), Self.families.contains(family) else { throw StripeError("Bulk encoding requires one to three records and an exact supported family.") }
        self.records = records; self.family = family; self.overwrite = overwrite; self.format = format
        encodedBytes = try NDEFCodec.encode(records).count
        let memory = family == .classic1k ? 720 : Int(try NDEFFormatting.cc(family)[2])*8
        guard encodedBytes + (encodedBytes < 255 ? 2 : 4) <= memory else { throw StripeError("Bulk records exceed the selected family's canonical capacity.") }
    }
    public func review(_ snapshot: ClassicSnapshot) throws -> NDEFBulkReview {
        guard snapshot.card.family == family else { throw StripeError("Target family does not match the frozen job.") }
        if let area = try? ClassicNDEF(snapshot:snapshot) {
            guard area.writable else { throw StripeError("Target NFC area is read-only.") }
            _ = try area.replacing(with:records)
            var remaining = Array(area.memory.dropFirst(area.prefix.count))
            let allNull = remaining.allSatisfy { $0 == 0 }
            if remaining.starts(with:[3,0]) {
                remaining.removeFirst(2)
                if remaining.first == 254 { remaining.removeFirst() }
            } else if !allNull { remaining = [1] }
            let empty = area.records.isEmpty && remaining.allSatisfy { $0 == 0 }
            guard overwrite || empty || area.records == records else { throw StripeError("Different existing records or hidden data require explicit overwrite permission.") }
            return NDEFBulkReview(snapshot:snapshot,area:area,requiresFormat:false,alreadyCorrect:area.records == records)
        }
        guard format else { throw StripeError("Target is not NFC formatted. Canonical formatting is disabled for this job.") }
        _ = try NDEFFormatting.snapshotBytes(snapshot,overwrite:overwrite)
        return NDEFBulkReview(snapshot:snapshot,area:nil,requiresFormat:true,alreadyCorrect:false)
    }
}

public struct NDEFBulkReview {
    public let id = UUID()
    public let snapshot: ClassicSnapshot
    public let area: ClassicNDEF?
    public let requiresFormat: Bool
    public let alreadyCorrect: Bool
}

public struct NDEFBulkState {
    public enum Phase: Equatable { case stopped, removalRequired, ready, review, writing, unverified }
    public private(set) var phase = Phase.stopped
    public private(set) var job: NDEFBulkJob?
    public private(set) var review: NDEFBulkReview?
    public private(set) var generation = UUID()
    public private(set) var succeeded = 0
    public private(set) var failed = 0
    public private(set) var alreadyCorrect = 0
    public init() {}
    public mutating func arm(_ job: NDEFBulkJob) {
        guard phase != .unverified, phase != .writing else { return }
        self.job = job; review = nil; generation = UUID(); phase = .removalRequired
        succeeded = 0; failed = 0; alreadyCorrect = 0
    }
    public mutating func presence(_ present: Bool) {
        if !present && phase == .removalRequired { phase = .ready }
        else if !present && phase == .review { review = nil; phase = .ready }
    }
    public mutating func inspect(_ snapshot: ClassicSnapshot) throws {
        guard phase == .ready, let job else { throw StripeError("Observe target removal before reviewing another target.") }
        do { review = try job.review(snapshot); phase = .review }
        catch { failed += 1; phase = .removalRequired; throw error }
    }
    public mutating func confirm(_ id: UUID, generation: UUID) throws -> NDEFBulkReview {
        guard phase == .review, self.generation == generation, let review, review.id == id else { throw StripeError("Bulk review expired. Review this target again.") }
        self.review = nil; phase = .writing
        return review
    }
    public mutating func finish(already: Bool = false, unverified: Bool = false) {
        guard phase == .writing else { return }
        if unverified { failed += 1; phase = .unverified }
        else { if already { alreadyCorrect += 1 } else { succeeded += 1 }; phase = .removalRequired }
    }
    public mutating func stop() {
        let uncertain = phase == .writing || phase == .unverified
        if phase == .writing { failed += 1 }
        generation = UUID(); review = nil; job = nil; phase = uncertain ? .unverified : .stopped
    }
}
