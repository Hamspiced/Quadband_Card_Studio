import Foundation
import MagStripeCore

extension NFCModel {
    func invalidateBulkDraft() {
        if ndefBulk.phase != .stopped && ndefBulk.phase != .unverified { stopNDEFBulk() }
    }
    var bulkDraftError: String? {
        do {
            _ = try NDEFBulkJob(records:recordQueue,family:bulkFamily,overwrite:bulkOverwrite,format:bulkFormat)
            guard !selectedReader.isEmpty, writableFamily(selectedReader,bulkFamily) else { return "Select a reader that supports the exact target family." }
            if bulkFormat && !formattingFamily(selectedReader,bulkFamily) { return "This reader does not advertise canonical formatting for the selected family." }
            _ = try HexBytes.parse(key,count:6)
            return nil
        } catch { return error.localizedDescription }
    }
    func armNDEFBulk() {
        guard !busy, bulkDraftError == nil else { return }
        do {
            let job = try NDEFBulkJob(records:recordQueue,family:bulkFamily,overwrite:bulkOverwrite,format:bulkFormat)
            ndefBulk.arm(job)
            guard ndefBulk.phase == .removalRequired else { return }
            bulkReader = selectedReader; bulkKey = key; bulkKeyType = keyType
            busy = true; status = "Bulk armed. Remove any card before presenting the first target."; statusKind = .working
            let generation = ndefBulk.generation, reader = bulkReader
            let credential = try HexBytes.parse(bulkKey,count:6), type = bulkKeyType
            bulkPolling = Task {
                while !Task.isCancelled && ndefBulk.generation == generation {
                    do {
                        guard selectedReader == reader, key == bulkKey, keyType == bulkKeyType else { stopNDEFBulk(); return }
                        if ndefBulk.phase != .writing {
                            bulkChecking = true
                            defer { bulkChecking = false }
                            let present = try await service.isPresent(reader:reader)
                            try Task.checkCancellation()
                            guard ndefBulk.generation == generation else { return }
                            ndefBulk.presence(present)
                            if present && ndefBulk.phase == .ready {
                                let target = try await service.readAutomatic(reader:reader,key:credential,type:type,sector:nil)
                                try Task.checkCancellation()
                                guard ndefBulk.generation == generation else { return }
                                try ndefBulk.inspect(target)
                                status = "Target read. Review and explicitly confirm this target; polling never writes."
                            } else if present, ndefBulk.phase == .review, let reviewed = ndefBulk.review {
                                guard try await service.identify(reader:reader) == reviewed.snapshot.card else { stopNDEFBulk(); return }
                            }
                        }
                        try await Task.sleep(nanoseconds:pollingInterval)
                    } catch {
                        if Task.isCancelled { return }
                        status = error.localizedDescription; statusKind = .error
                        // A transport/authentication error never counts as removal.
                        if ndefBulk.phase != .removalRequired { stopNDEFBulk(); return }
                        do { try await Task.sleep(nanoseconds:pollingInterval) } catch { return }
                    }
                }
            }
        } catch { failed(error) }
    }
    func stopNDEFBulk() {
        bulkPolling?.cancel(); bulkPolling = nil; operation?.cancel()
        ndefBulk.stop()
        if !writing { busy = false }
        status = ndefBulk.phase == .unverified ? "Bulk stopped — target outcome unverified. No automatic retry. Read and inspect the target separately." : "Bulk stopped."
        statusKind = ndefBulk.phase == .unverified ? .error : .neutral
    }
    func confirmNDEFBulk(_ id: UUID, generation: UUID) {
        guard !bulkChecking else { return }
        guard !writing, selectedReader == bulkReader, key == bulkKey, keyType == bulkKeyType,
              let job = ndefBulk.job else { stopNDEFBulk(); return }
        let reviewed: NDEFBulkReview, credential: [UInt8]
        do { credential = try HexBytes.parse(bulkKey,count:6); reviewed = try ndefBulk.confirm(id,generation:generation) }
        catch { failed(error); return }
        let reader = bulkReader, type = bulkKeyType
        writing = true
        operation = Task {
            defer { writing = false; operation = nil; if ndefBulk.phase == .unverified || ndefBulk.phase == .stopped { busy = false } }
            do {
                var target = try await service.readAutomatic(reader:reader,key:credential,type:type,sector:nil)
                try Task.checkCancellation()
                guard ndefBulk.generation == generation, target.card == reviewed.snapshot.card,
                      target.blocks == reviewed.snapshot.blocks, target.sectorKeys == reviewed.snapshot.sectorKeys,
                      target.ntagAuthenticated == reviewed.snapshot.ntagAuthenticated else { throw StripeError("Target or permissions changed since review.") }
                let fresh = try job.review(target)
                if fresh.requiresFormat {
                    target = try await service.formatNDEF(reader:reader,expected:target,overwrite:job.overwrite)
                    guard target.card == reviewed.snapshot.card else { throw StripeError("Target changed after formatting.") }
                }
                try Task.checkCancellation()
                guard ndefBulk.generation == generation else { throw CancellationError() }
                if !fresh.alreadyCorrect {
                    let area = try ClassicNDEF(snapshot:target)
                    target = try await service.writeNDEF(reader:reader,records:job.records,expected:area,key:credential,type:type)
                }
                guard ndefBulk.generation == generation, target.card == reviewed.snapshot.card,
                      try ClassicNDEF(snapshot:target).records == job.records else { throw StripeError("Bulk final verification mismatch.") }
                ndefBulk.finish(already:fresh.alreadyCorrect)
                status = fresh.alreadyCorrect ? "Already correct. Remove this target before continuing." : "Verified. Remove this target before continuing."
                statusKind = .success
            } catch {
                ndefBulk.finish(unverified:true)
                bulkPolling?.cancel(); bulkPolling = nil
                status = "Bulk target unverified; stopped without retry. \(error.localizedDescription)"; statusKind = .error
            }
        }
    }
}
