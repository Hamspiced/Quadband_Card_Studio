// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import AppKit
import SwiftUI

struct KeyboardCapture: NSViewRepresentable {
    let armed: Bool
    let generation: Int
    let onSwipe: (String) -> Void
    let onError: (String) -> Void
    func makeNSView(context: Context) -> SwipeCaptureView { SwipeCaptureView() }
    func updateNSView(_ view: SwipeCaptureView, context: Context) {
        view.onSwipe = onSwipe; view.onError = onError
        view.setArmed(armed, generation: generation)
    }
}

final class SwipeCaptureView: NSView {
    var onSwipe: ((String) -> Void)?
    var onError: ((String) -> Void)?
    private var armed = false
    private var generation = -1
    private var buffer = ""
    private var completion: DispatchWorkItem?
    override var acceptsFirstResponder: Bool { true }
    func setArmed(_ value: Bool, generation: Int) {
        let changed = self.generation != generation
        self.generation = generation
        if !value { armed = false; buffer = ""; completion?.cancel(); return }
        if !armed || changed {
            armed = true; buffer = ""; completion?.cancel()
            DispatchQueue.main.async { [weak self] in
                guard let self, self.armed else { return }
                self.window?.makeFirstResponder(self)
            }
        }
    }
    override func keyDown(with event: NSEvent) {
        guard armed else { super.keyDown(with: event); return }
        if event.keyCode == 53 { fail("Reader capture cancelled."); return }
        guard !event.modifierFlags.contains(.command), !event.modifierFlags.contains(.control) else { return }
        guard let text = event.characters else { return }
        buffer += text
        guard buffer.utf8.count <= 1024 else { fail("Swipe exceeds the supported length. Clear and try again."); return }
        completion?.cancel()
        let item = DispatchWorkItem { [weak self] in self?.finish() }
        completion = item
        // Accommodate readers with CR between tracks as well as a final CR.
        let delay = text.contains(where: \.isNewline) ? 0.3 : 0.8
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: item)
    }
    override func resignFirstResponder() -> Bool {
        if armed { fail("Reader capture stopped because the input focus changed. Click Start reading and swipe again.") }
        return super.resignFirstResponder()
    }
    private func finish() {
        guard armed, !buffer.isEmpty else { return }
        let value = buffer
        armed = false; buffer = ""
        onSwipe?(value)
    }
    private func fail(_ message: String) {
        armed = false; buffer = ""; completion?.cancel()
        DispatchQueue.main.async { [weak self] in self?.onError?(message) }
    }
}
