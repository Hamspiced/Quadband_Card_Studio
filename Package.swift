// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MagStripeStudio",
    platforms: [.macOS(.v13)],
    products: [.executable(name: "MagStripeStudio", targets: ["MagStripeStudio"])],
    targets: [
        .target(name: "MagStripeCore"),
        .target(name: "CPCSC", linkerSettings: [.linkedFramework("PCSC")]),
        .executableTarget(name: "MagStripeStudio", dependencies: ["MagStripeCore", "CPCSC"],
                          linkerSettings: [.linkedFramework("IOKit"), .linkedFramework("AppKit"), .linkedFramework("CoreBluetooth")]),
        .testTarget(name: "MagStripeCoreTests", dependencies: ["MagStripeCore"]),
        .testTarget(name: "MagStripeAppTests", dependencies: ["MagStripeStudio", "MagStripeCore"])
    ]
)
