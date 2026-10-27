// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main struct MagStripeStudioApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = AppModel()
    var body: some Scene {
        WindowGroup("Quadband Card Studio by Midwest Gadgets") {
            ContentView().environmentObject(model)
                .frame(minWidth: 1040, minHeight: 760)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in model.disconnect(silent: true); model.uhf.disconnect(silent:true); model.lf.close() }
                .onReceive(NotificationCenter.default.publisher(for: NSWindow.didResignKeyNotification)) { _ in
                    if model.keyboardArmed { model.stopKeyboard() }
                }
        }
        .defaultSize(width: 1180, height: 860)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Card") {
                Button("Load short test data") { model.loadPreset(original: false) }.disabled(model.busy)
                Button("Load Midwest test data") { model.loadPreset(original: true) }.disabled(model.busy)
                Divider()
                Button("Clear session") { model.clearSession() }.disabled(model.busy)
            }
        }
    }
}
