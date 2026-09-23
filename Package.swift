// swift-tools-version: 6.0
import PackageDescription

var appDependencies: [Package.Dependency] = []
var labTestDependencies: [Target.Dependency] = ["SpatialSnapshot", "SpatialSnapshotCapture", "SpatialSnapshotAppleMedia", "SpatialSnapshotRealityKit"]
#if os(macOS)
appDependencies.append(.package(path: "Apps/SpatialSnapshotLab/Vendor/GLTFKit2"))
labTestDependencies.append(.product(name: "GLTFKit2", package: "GLTFKit2"))
#endif

let package = Package(
    name: "SpatialSnapshot",
    platforms: [.macOS(.v13), .iOS(.v16)],
    products: [
        .library(name: "SpatialSnapshotC", targets: ["SpatialSnapshotC"]),
        .library(name: "SpatialSnapshot", targets: ["SpatialSnapshot"]),
        .library(name: "SpatialSnapshotAppleMedia", targets: ["SpatialSnapshotAppleMedia"]),
        .library(name: "SpatialSnapshotCapture", targets: ["SpatialSnapshotCapture"]),
        .library(name: "SpatialSnapshotRealityKit", targets: ["SpatialSnapshotRealityKit"])
    ],
    dependencies: appDependencies,
    targets: [
        .target(name: "SpatialSnapshotC", publicHeadersPath: "include"),
        .target(name: "SpatialSnapshot", dependencies: ["SpatialSnapshotC"]),
        .target(name: "SpatialSnapshotAppleMedia", dependencies: ["SpatialSnapshot", "SpatialSnapshotC"],
                resources: [.process("Resources")]),
        .target(name: "SpatialSnapshotCapture", dependencies: ["SpatialSnapshot", "SpatialSnapshotAppleMedia"],
                resources: [.process("Resources")]),
        .target(name: "SpatialSnapshotRealityKit", dependencies: ["SpatialSnapshot"]),
        .testTarget(name: "SpatialSnapshotTests", dependencies: ["SpatialSnapshot"], path: "Tests/SwiftTests"),
        .testTarget(name: "SpatialSnapshotAppleTests", dependencies: ["SpatialSnapshotAppleMedia", "SpatialSnapshotCapture", "SpatialSnapshotRealityKit"], path: "Tests/AppleTests"),
        .testTarget(name: "SpatialSnapshotLabTests", dependencies: labTestDependencies,
                    path: "Apps/SpatialSnapshotLab",
                    exclude: ["Info.plist", "README.md", "Configuration", "Resources", "Vendor", "SpatialSnapshotLab.xcodeproj",
                              "Sources/SpatialSnapshotLabApp.swift"],
                    sources: ["Sources/InspectorPresentation.swift", "Sources/LabModel.swift", "Sources/Editor",
                              "Sources/LabRootView.swift", "Sources/InspectorView.swift", "Sources/SpatialSceneView.swift",
                              "Sources/RecorderView.swift", "Sources/SpatialCaptureShare.swift", "Tests"])
    ],
    cLanguageStandard: .c17
)
