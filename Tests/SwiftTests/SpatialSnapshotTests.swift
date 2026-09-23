import Foundation
import XCTest
@testable import SpatialSnapshot

final class SpatialSnapshotTests: XCTestCase {
    private func camera(_ time: UInt64 = 0) -> Camera {
        Camera(timestampNanoseconds: time, width: 4, height: 3, fx: 2, fy: 3, cx: 1.5, cy: 1)
    }
    func testStillWithGeometryAndDepth() throws {
        let chunks = try MeshChunk.partition([SceneTriangle(SIMD3(0, 0, 1), SIMD3(0.5, 0, 1), SIMD3(0, 0.5, 1), classification: .table)])
        let writer = try SpatialWriter(kind: .still)
        let depth = try DepthSample(timestampNanoseconds: 0, width: 1, height: 1, fx: 1, fy: 1, cx: 0, cy: 0,
                                    millimeters: [1000], confidence: [3])
        try writer.append(camera: camera(), geometry: chunks.map(GeometryUpdate.put), depth: depth)
        let bytes = try writer.finish(durationNanoseconds: 0)
        let document = try SpatialDocument(data: bytes)
        XCTAssertEqual(document.kind, .still)
        XCTAssertEqual(document.durationNanoseconds, 0)
        let scene = try document.scene()
        let hit = try XCTUnwrap(scene.raycast(pixel: SIMD2(1.75, 1.75)))
        XCTAssertEqual(hit.position.z, 1, accuracy: 1e-10)
        XCTAssertEqual(hit.classification, .table)
        XCTAssertEqual(try scene.depth()?.millimeters, [1000])
        XCTAssertEqual(try scene.depthPoint(u: 0, v: 0)?.point.z, 1)
        XCTAssertEqual(try scene.meshes().first?.positions().count, 3)
        XCTAssertEqual(try document.packets().count, 7)
        try document.validateMediaBinding(width: 4, height: 3, presentationTimes: [0], duration: 0)
    }
    func testVideoTrimAndInstantaneousSamples() throws {
        let chunks = try MeshChunk.partition([SceneTriangle(SIMD3(0, 0, 1), SIMD3(0.5, 0, 1), SIMD3(0, 0.5, 1))])
        let writer = try SpatialWriter(kind: .video)
        try writer.append(camera: camera(), geometry: chunks.map(GeometryUpdate.put))
        var next = camera(100)
        next.translation.x = 0.25
        try writer.append(camera: next)
        let original = try SpatialDocument(data: writer.finish(durationNanoseconds: 200))
        XCTAssertNil(try original.scene(at: 50).camera())
        XCTAssertNil(try original.scene(at: 100).depth())
        let trimmed = try SpatialDocument(data: original.trim(from: 100, to: 200))
        XCTAssertNotEqual(original.streamID, trimmed.streamID)
        XCTAssertEqual(trimmed.durationNanoseconds, 100)
        XCTAssertEqual(try trimmed.cameras().first?.translation, .zero)
        XCTAssertEqual(try trimmed.scene().meshes().count, 2)
        XCTAssertThrowsError(try original.trim(from: 50, to: 200))
    }
    func testLargeMeshRoundTripsForStillAndVideo() throws {
        // 131,072 distinct faces exceed both the former capture default (50,000)
        // and its configurable ceiling (100,000). Use the same C budgets as capture.
        let side = 256
        var triangles: [SceneTriangle] = []
        triangles.reserveCapacity(2 * side * side)
        for y in 0..<side {
            for x in 0..<side {
                let a = SIMD3(Double(x) / 64, Double(y) / 64, 1.25)
                let b = SIMD3(Double(x + 1) / 64, Double(y) / 64, 1.25)
                let c = SIMD3(Double(x) / 64, Double(y + 1) / 64, 1.25)
                let d = SIMD3(Double(x + 1) / 64, Double(y + 1) / 64, 1.25)
                let classification: SurfaceClassification = (x + y).isMultiple(of: 2) ? .wall : .window
                triangles.append(try SceneTriangle(a, b, c, classification: classification))
                triangles.append(try SceneTriangle(b, d, c, classification: classification))
            }
        }
        let chunks = try MeshChunk.partition(triangles)
        XCTAssertEqual(chunks.count, 64)
        XCTAssertEqual(chunks.reduce(0) { $0 + $1.triangles.count }, triangles.count)
        for kind: StreamKind in [.still, .video] {
            let writer = try SpatialWriter(kind: kind)
            try writer.append(camera: camera(), geometry: chunks.map(GeometryUpdate.put))
            if kind == .video { try writer.append(camera: camera(100)) }
            let document = try SpatialDocument(data: writer.finish(durationNanoseconds: kind == .still ? 0 : 200))
            let decoded = try document.scene(at: kind == .still ? 0 : 100).meshes()
            XCTAssertEqual(decoded.reduce(0) { $0 + $1.triangles.count }, triangles.count)
            let byCell = Dictionary(uniqueKeysWithValues: decoded.map { ($0.cell, $0) })
            XCTAssertEqual(byCell.count, chunks.count)
            for chunk in chunks {
                let saved = try XCTUnwrap(byCell[chunk.cell])
                XCTAssertEqual(saved.quantizedVertices, chunk.quantizedVertices)
                XCTAssertEqual(saved.triangles, chunk.triangles)
                XCTAssertEqual(saved.classifications, chunk.classifications)
            }
        }
    }
    func testFragmentationAndMalformedError() throws {
        let writer = try SpatialWriter(kind: .still)
        try writer.append(camera: camera())
        let data = try writer.finish(durationNanoseconds: 0)
        let decoder = try SpatialStreamDecoder()
        for byte in data { try decoder.feed(Data([byte])) }
        XCTAssertEqual(try decoder.finish().kind, .still)
        XCTAssertThrowsError(try SpatialDocument(data: data.dropLast())) { error in
            XCTAssertEqual((error as? SpatialSnapshotError)?.code, -2)
            XCTAssertNotNil((error as? SpatialSnapshotError)?.byteOffset)
        }
    }
    func testProjectionAndRounding() throws {
        let c = camera()
        let point = try c.unproject(SIMD2(2, 1), depthMeters: 2)
        XCTAssertEqual(point.z, 2)
        XCTAssertEqual(try c.project(point), SIMD2(2, 1))
        XCTAssertEqual(try presentationNanoseconds(value: 1, timescale: 2_000_000_000), 1)
        XCTAssertThrowsError(try presentationNanoseconds(value: .max, timescale: 1))
    }
}
