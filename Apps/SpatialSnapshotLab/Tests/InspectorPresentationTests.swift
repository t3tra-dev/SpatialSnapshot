#if canImport(CoreGraphics) && canImport(simd)
import CoreGraphics
import simd
import XCTest
import SpatialSnapshot
import SpatialSnapshotCapture

final class InspectorPresentationTests: XCTestCase {
    func testPortraitCaptureIsValidAndFloorStaysHorizontalInViewer() throws {
        // Native sensor +X points down in world when the device is held in portrait.
        let first = simd_float4x4(simd_quatf(angle: -.pi / 2, axis: SIMD3(0, 0, 1)))
        let basis = CaptureCoordinates(firstCameraTransform: first)
        let intrinsics = simd_float3x3(SIMD3(500, 0, 0), SIMD3(0, 520, 0), SIMD3(319.5, 239.5, 1))
        let camera = basis.camera(transform: first, intrinsics: intrinsics,
            width: 640, height: 480, timestamp: 0, isFirst: true)
        XCTAssertEqual(camera.translation, .zero)
        XCTAssertEqual(camera.quaternion, SIMD4(0, 0, 0, 1))
        XCTAssertEqual(basis.gravity.x, 1, accuracy: 0.00001)
        XCTAssertEqual(InspectorPresentation.initialQuarterTurns(gravity: basis.gravity), 1)
        let floor = [SIMD3<Float>(-1, -1, -1), SIMD3(1, -1, -1), SIMD3(-1, -1, -3)]
        let wall = [SIMD3<Float>(-1, -1, -3), SIMD3(1, -1, -3), SIMD3(-1, 1, -3)]
        func scenePoint(_ point: SIMD3<Float>) -> SIMD3<Double> {
            let p = basis.sceneFromWorld * SIMD4(point.x, point.y, point.z, 1)
            return SIMD3(Double(p.x), Double(p.y), Double(p.z))
        }
        let chunks = try MeshChunk.partition([
            SceneTriangle(scenePoint(floor[0]), scenePoint(floor[1]), scenePoint(floor[2]), classification: .floor),
            SceneTriangle(scenePoint(wall[0]), scenePoint(wall[1]), scenePoint(wall[2]), classification: .wall)
        ])
        let writer = try SpatialWriter(kind: .still, gravity: basis.gravity)
        try writer.append(camera: camera, geometry: chunks.map(GeometryUpdate.put))
        let document = try SpatialDocument(data: writer.finish(durationNanoseconds: 0))
        let orientation = InspectorPresentation.sceneOrientation(gravity: document.gravity)
        for chunk in try document.scene().meshes() {
            let points = try chunk.positions().map {
                orientation.act(SIMD3<Float>(Float($0.x), -Float($0.y), -Float($0.z)))
            }
            for (i, triangle) in chunk.triangles.enumerated() {
                for index in [triangle.x, triangle.y, triangle.z] {
                    if chunk.classifications[i] == .floor {
                        XCTAssertEqual(points[Int(index)].y, -1, accuracy: 0.00002)
                    } else {
                        XCTAssertEqual(points[Int(index)].z, -3, accuracy: 0.00002)
                    }
                }
            }
        }
    }

    func testQuarterTurnsAndGravityAlignmentInEveryOrientation() {
        let directions: [(SIMD3<Double>, Int)] = [
            (SIMD3(0, 1, 0), 0), (SIMD3(1, 0, 0), 1),
            (SIMD3(0, -1, 0), 2), (SIMD3(-1, 0, 0), 3),
            (SIMD3(0, 0, 1), 0), (SIMD3(0, 0, -1), 0),
            (simd_normalize(SIMD3(1, 2, 3)), 0)
        ]
        for (gravity, turns) in directions {
            XCTAssertEqual(InspectorPresentation.initialQuarterTurns(gravity: gravity), turns)
            let rotation = InspectorPresentation.sceneOrientation(gravity: gravity)
            let down = rotation.act(SIMD3<Float>(Float(gravity.x), -Float(gravity.y), -Float(gravity.z)))
            XCTAssertLessThan(simd_length(down - SIMD3(0, -1, 0)), 0.00001)
            XCTAssertEqual(simd_determinant(simd_float3x3(rotation)), 1, accuracy: 0.00001)
        }
        let upsideDown = InspectorPresentation.sceneOrientation(gravity: SIMD3(0, -1, 0))
        XCTAssertLessThan(simd_length(upsideDown.act(SIMD3(0, 0, -1)) - SIMD3(0, 0, -1)), 0.00001)
    }

    func testRotatedPixelCentersAndRaycastStayAligned() throws {
        let camera = Camera(width: 640, height: 480, fx: 500, fy: 520, cx: 319.5, cy: 239.5)
        let chunks = try MeshChunk.partition([
            SceneTriangle(SIMD3(-0.5, -0.5, 2), SIMD3(1, -0.5, 2), SIMD3(0, 1, 2), classification: .wall)
        ])
        let writer = try SpatialWriter(kind: .still)
        try writer.append(camera: camera, geometry: chunks.map(GeometryUpdate.put))
        let document = try SpatialDocument(data: writer.finish(durationNanoseconds: 0))
        let scene = try document.scene()
        let point = SIMD3(0.1, 0.2, 2.0)
        let projected = try camera.project(point)
        for turns in 0..<4 {
            for viewport in [CGSize(width: 301, height: 419), CGSize(width: 723, height: 317)] {
                let layout = InspectorRasterLayout(rasterSize: CGSize(width: 640, height: 480),
                    viewportSize: viewport, quarterTurns: turns)
                for pixel in [CGPoint.zero, CGPoint(x: 639, y: 479), CGPoint(x: 319.5, y: 239.5),
                              CGPoint(x: projected.x, y: projected.y)] {
                    let decoded = try XCTUnwrap(layout.pixel(at: layout.viewPoint(for: pixel)))
                    XCTAssertEqual(decoded.x, pixel.x, accuracy: 0.000001)
                    XCTAssertEqual(decoded.y, pixel.y, accuracy: 0.000001)
                }
                XCTAssertNil(layout.pixel(at: CGPoint(x: -1, y: -1)))
                XCTAssertNil(layout.pixel(at: CGPoint(x: layout.contentRect.minX - 1, y: layout.contentRect.midY)))
                let tap = layout.viewPoint(for: CGPoint(x: projected.x, y: projected.y))
                let pixel = try XCTUnwrap(layout.pixel(at: tap))
                let hit = try XCTUnwrap(scene.raycast(pixel: SIMD2(Double(pixel.x), Double(pixel.y))))
                XCTAssertLessThan(simd_length(hit.position - point), 0.00001)
                XCTAssertEqual(hit.classification, .wall)
            }
        }
        // Independent corner expectations catch a consistently reversed forward/inverse mapping.
        let portrait = InspectorRasterLayout(rasterSize: CGSize(width: 640, height: 480),
            viewportSize: CGSize(width: 480, height: 640), quarterTurns: 1)
        XCTAssertEqual(portrait.viewPoint(for: .zero), CGPoint(x: 479.5, y: 0.5))
        XCTAssertEqual(portrait.viewPoint(for: CGPoint(x: 639, y: 479)), CGPoint(x: 0.5, y: 639.5))
        let empty = InspectorRasterLayout(rasterSize: CGSize(width: 640, height: 480),
            viewportSize: .zero, quarterTurns: 1)
        XCTAssertNil(empty.pixel(at: .zero))
    }
}
#endif
