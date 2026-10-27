import AppKit

guard let image = NSImage(contentsOfFile: "Resources/MidwestGadgets.png") else {
    fatalError("Missing Midwest Gadgets logo")
}
let folder = URL(fileURLWithPath: "Resources/AppIcon.iconset")
try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        image.draw(in: NSRect(x: 0, y: 0, width: pixels, height: pixels))
        NSGraphicsContext.restoreGraphicsState()
        try rep.representation(using: .png, properties: [:])!.write(to: folder.appendingPathComponent("icon_\(size)x\(size)\(scale == 2 ? "@2x" : "").png"))
    }
}
let task = Process(); task.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil"); task.arguments = ["-c", "icns", folder.path, "-o", "Resources/AppIcon.icns"]
try task.run(); task.waitUntilExit()
guard task.terminationStatus == 0 else { fatalError("Icon generation failed") }
