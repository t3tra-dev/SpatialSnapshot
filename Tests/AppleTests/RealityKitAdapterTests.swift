#if canImport(RealityKit)
import XCTest
import RealityKit
import SpatialSnapshot
import SpatialSnapshotRealityKit
import SpatialSnapshotCapture

final class RealityKitAdapterTests: XCTestCase {
    @MainActor
    func testMeshOcclusionAndPlacementUseCanonicalAxes() throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Requires Apple rendering services; enable with the Apple media tests.")
        }
        let writer = try SpatialWriter(kind: .still)
        let chunks = try MeshChunk.partition([
            SceneTriangle(SIMD3(0, 0, 1), SIMD3(0, 0.5, 1), SIMD3(0.5, 0, 1), classification: .table)
        ])
        try writer.append(camera: Camera(width: 200, height: 200, fx: 200, fy: 200, cx: 99.5, cy: 99.5),
                          geometry: chunks.map { .put($0) })
        let document = try SpatialDocument(data: writer.finish(durationNanoseconds: 0))
        let scene = try document.scene()
        let entity = try SpatialSnapshotRealityKit.meshEntity(from: scene)
        let bounds = entity.visualBounds(relativeTo: nil)
        XCTAssertEqual(bounds.center.x, 0.25, accuracy: 0.0001)
        XCTAssertEqual(bounds.center.y, -0.25, accuracy: 0.0001)
        XCTAssertEqual(bounds.center.z, -1, accuracy: 0.0001)
        let occlusion = try SpatialSnapshotRealityKit.occlusionEntity(from: scene)
        XCTAssertFalse(occlusion.children.isEmpty)
        XCTAssertTrue((occlusion.children.first as? ModelEntity)?.model?.materials.first is OcclusionMaterial)
        let hit = try XCTUnwrap(scene.raycast(pixel: SIMD2(119.5, 119.5)))
        let marker = SpatialSnapshotRealityKit.placementEntity(at: hit)
        XCTAssertEqual(marker.position.x, 0.1, accuracy: 0.0001)
        XCTAssertEqual(marker.position.y, -0.1, accuracy: 0.0001)
        XCTAssertEqual(marker.position.z, -1, accuracy: 0.0001)
    }

    @MainActor
    func testDebugMeshMatchesSavedTrianglesAndCaptureWorld() throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Requires Apple rendering services; enable with the Apple media tests.")
        }
        // Cross cell boundaries and quantize non-grid coordinates, then use the read-back
        // document as an independent reference for the geometry that is actually saved.
        let chunks = try MeshChunk.partition([
            SceneTriangle(SIMD3(-0.2, 0.13, 1.37), SIMD3(0.7, 0.16, 1.37),
                          SIMD3(0.24, 0.8, 1.37), classification: .wall),
            SceneTriangle(SIMD3(-0.1, 0.13, 1.87), SIMD3(0.2, 0.13, 1.87),
                          SIMD3(0.1, 0.3, 1.87), classification: .table)
        ])
        var first = simd_float4x4(simd_quatf(angle: 0.6, axis: simd_normalize(SIMD3<Float>(1, 2, 0))))
        first.columns.3 = SIMD4(2, 3, -4, 1)
        let coordinates = CaptureCoordinates(firstCameraTransform: first)
        let snapshot = CaptureMeshSnapshot(chunks: chunks, coordinates: coordinates)
        let writer = try SpatialWriter(kind: .still, gravity: coordinates.gravity)
        try writer.append(camera: Camera(width: 640, height: 480, fx: 520, fy: 520, cx: 319.5, cy: 239.5),
                          geometry: chunks.map { .put($0) })
        let document = try SpatialDocument(data: writer.finish(durationNanoseconds: 0))
        let saved = try document.scene().meshes()
        func key(_ points: [SIMD3<Float>]) -> [SIMD3<Float>] {
            points.sorted { a, b in
                a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z
            }
        }
        var expected: [[SIMD3<Float>]] = []
        for chunk in saved {
            let positions = try chunk.positions().map { SIMD3<Float>(Float($0.x), Float($0.y), Float($0.z)) }
            for triangle in chunk.triangles {
                expected.append(key([positions[Int(triangle.x)], positions[Int(triangle.y)], positions[Int(triangle.z)]]))
            }
        }
        let debug = try SpatialSnapshotRealityKit.debugMeshEntity(from: snapshot.chunks)
        debug.transform.matrix = snapshot.worldFromScene
        var actual: [[SIMD3<Float>]] = []
        for child in debug.children {
            let model = try XCTUnwrap((child as? ModelEntity)?.model)
            let material = try XCTUnwrap(model.materials.first as? UnlitMaterial)
            if #available(iOS 18, macOS 15, *) {
                XCTAssertEqual(material.triangleFillMode, .lines)
                XCTAssertFalse(material.readsDepth)
                XCTAssertFalse(material.writesDepth)
            }
            for model in model.mesh.contents.models {
                for part in model.parts {
                    let indices = try XCTUnwrap(part.triangleIndices).elements
                    let positions = part.positions.elements
                    for i in stride(from: 0, to: indices.count, by: 3) {
                        actual.append(key((i..<(i + 3)).map { positions[Int(indices[$0])] }))
                    }
                    for point in positions {
                        let p = SIMD4<Float>(point.x, point.y, point.z, 1)
                        let expectedWorld = first * SIMD4<Float>(point.x, -point.y, -point.z, 1)
                        let actualWorld = child.transformMatrix(relativeTo: nil) * p
                        XCTAssertLessThan(simd_length(actualWorld - expectedWorld), 0.00001)
                    }
                }
            }
        }
        XCTAssertEqual(actual.count, snapshot.triangleCount)
        XCTAssertEqual(actual.count, expected.count)
        XCTAssertEqual(Set(actual), Set(expected))
        let empty = try SpatialSnapshotRealityKit.debugMeshEntity(from: [])
        XCTAssertTrue(empty.children.isEmpty)
    }
}
#endif
