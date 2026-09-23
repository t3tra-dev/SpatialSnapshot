#if canImport(SwiftUI)
import Foundation
import XCTest
import SpatialSnapshot

final class CaptureDeletionTests: XCTestCase {
    private func makeDirectory() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("SpatialSnapshot-deletion-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }
    private func makeCapture(in directory: URL, name: String) throws -> URL {
        let url = directory.appendingPathComponent(name + ".ssps")
        let writer = try SpatialWriter(kind: .video)
        let chunks = try MeshChunk.partition([
            SceneTriangle(SIMD3(0, 0, 1), SIMD3(0.5, 0, 1), SIMD3(0, 0.5, 1), classification: .table)
        ])
        var camera = Camera(width: 4, height: 3, fx: 2, fy: 3, cx: 1.5, cy: 1)
        try writer.append(camera: camera, geometry: chunks.map(GeometryUpdate.put))
        camera.timestampNanoseconds = 1_000_000_000
        try writer.append(camera: camera)
        try writer.finish(durationNanoseconds: 2_000_000_000).write(to: url)
        return url
    }

    @MainActor
    func testDeletingOpenCaptureStopsPlaybackAndClearsInspector() async throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let removed = try makeCapture(in: directory, name: "Capture-current")
        let retained = try makeCapture(in: directory, name: "Capture-retained")
        let model = LabModel(libraryDirectory: directory)
        await model.open(removed)
        XCTAssertNil(model.error)
        XCTAssertGreaterThan(model.meshCells, 0)
        XCTAssertFalse(model.edges.isEmpty)
        model.togglePlayback()
        XCTAssertTrue(model.isPlaying)

        model.deleteCapture(removed)

        XCTAssertFalse(FileManager.default.fileExists(atPath: removed.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: retained.path))
        XCTAssertEqual(model.recentFiles.map(\.lastPathComponent), [retained.lastPathComponent])
        XCTAssertNil(model.asset)
        XCTAssertNil(model.scene)
        XCTAssertNil(model.image)
        XCTAssertNil(model.depthImage)
        XCTAssertNil(model.hit)
        XCTAssertNil(model.hitPixel)
        XCTAssertTrue(model.edges.isEmpty)
        XCTAssertEqual(model.meshCells, 0)
        XCTAssertEqual(model.triangleCount, 0)
        XCTAssertEqual(model.frameCount, 0)
        XCTAssertFalse(model.isPlaying)
        XCTAssertNil(model.error)
        await Task.yield()
        XCTAssertNil(model.asset)
        XCTAssertNil(model.image)
    }

    @MainActor
    func testDeletingAnotherCapturePreservesInspectorAndBusyOperation() async throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let opened = try makeCapture(in: directory, name: "Capture-open")
        let removed = try makeCapture(in: directory, name: "Capture-other")
        let model = LabModel(libraryDirectory: directory)
        await model.open(opened)
        let asset = try XCTUnwrap(model.asset)
        let revision = model.revision
        model.layer = .scene

        model.deleteCapture(removed)

        XCTAssertTrue(model.asset === asset)
        XCTAssertEqual(model.revision, revision)
        XCTAssertEqual(model.layer, .scene)
        XCTAssertEqual(model.recentFiles.map(\.lastPathComponent), [opened.lastPathComponent])
        XCTAssertTrue(FileManager.default.fileExists(atPath: opened.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: removed.path))
        model.isBusy = true
        model.deleteCapture(opened)
        XCTAssertTrue(FileManager.default.fileExists(atPath: opened.path))
        XCTAssertTrue(model.asset === asset)
        XCTAssertNil(model.error)
    }

    @MainActor
    func testFailedDeletionKeepsEntryAndInspector() async throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = try makeCapture(in: directory, name: "Capture-missing")
        let model = LabModel(libraryDirectory: directory)
        await model.open(url)
        let asset = try XCTUnwrap(model.asset)
        let revision = model.revision
        try FileManager.default.removeItem(at: url)

        model.deleteCapture(url)

        XCTAssertNotNil(model.error)
        XCTAssertTrue(model.asset === asset)
        XCTAssertEqual(model.revision, revision)
        XCTAssertEqual(model.recentFiles.map(\.lastPathComponent), [url.lastPathComponent])
    }

    @MainActor
    func testOnlyLibraryFilesCanBeDeleted() throws {
        let root = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("Library", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let external = try makeCapture(in: root, name: "External")
        let nested = directory.appendingPathComponent("Folder.ssps", isDirectory: true)
        try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true)
        let contents = try makeCapture(in: nested, name: "Retained")
        let model = LabModel(libraryDirectory: directory)

        model.deleteCapture(external)
        XCTAssertTrue(FileManager.default.fileExists(atPath: external.path))
        XCTAssertNotNil(model.error)
        model.error = nil
        model.deleteCapture(nested)
        XCTAssertTrue(FileManager.default.fileExists(atPath: contents.path))
        XCTAssertNotNil(model.error)
        let link = directory.appendingPathComponent("Capture-link.ssps")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: external)
        model.refreshFiles()
        model.error = nil
        model.deleteCapture(link)
        XCTAssertFalse(FileManager.default.fileExists(atPath: link.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: external.path))
        XCTAssertNil(model.error)
    }
}
#endif
