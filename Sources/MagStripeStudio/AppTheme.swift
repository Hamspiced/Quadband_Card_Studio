// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import AppKit

enum AppearanceMode: String, CaseIterable {
    case system = "System", light = "Light", dark = "Dark"

    var colorScheme: ColorScheme? {
        switch self {
        case .system: return nil
        case .light: return .light
        case .dark: return .dark
        }
    }
}

enum AppTheme {
    private static func adaptive(light: NSColor, dark: NSColor) -> Color {
        Color(nsColor: NSColor(name: nil) { appearance in
            appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light
        })
    }

    static let canvas = adaptive(
        light: NSColor(srgbRed: 0.96, green: 0.97, blue: 0.98, alpha: 1),
        dark: NSColor(srgbRed: 0.07, green: 0.11, blue: 0.15, alpha: 1))
    static let surface = adaptive(
        light: .white,
        dark: NSColor(srgbRed: 0.11, green: 0.16, blue: 0.21, alpha: 1))
    static let accent = adaptive(
        light: NSColor(srgbRed: 0, green: 0.43, blue: 0.40, alpha: 1),
        dark: NSColor(srgbRed: 0.33, green: 0.82, blue: 0.72, alpha: 1))
}
