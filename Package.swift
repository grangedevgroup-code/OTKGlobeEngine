// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "OTKGlobeEngine",
    platforms: [.macOS(.v12), .iOS(.v15)],
    products: [
        .library(name: "OTKGlobeView", targets: ["OTKGlobeView"]),
    ],
    targets: [
        .systemLibrary(name: "OTKGlobeEngineC", path: "bindings/apple/swiftpm"),
        .target(
            name: "OTKGlobeView",
            dependencies: ["OTKGlobeEngineC"],
            path: "bindings/apple",
            exclude: ["include", "swiftpm"],
            sources: ["OTKGlobeView.swift"]
        ),
    ]
)
