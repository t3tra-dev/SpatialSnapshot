#if canImport(simd)
import XCTest
import simd
import SpatialSnapshot
@testable import SpatialSnapshotCapture

final class CaptureConversionTests: XCTestCase {
    func testFirstCameraBasisAndGravity() throws {
        var first = matrix_identity_float4x4
        first.columns.3 = SIMD4(2, 3, 4, 1)
        let coordinates = CaptureCoordinates(firstCameraTransform: first)
        let intrinsics = simd_float3x3(SIMD3(500, 0, 0), SIMD3(0, 500, 0), SIMD3(320, 240, 1))
        let camera = coordinates.camera(transform: first, intrinsics: intrinsics, width: 640, height: 480, timestamp: 0, isFirst: true)
        XCTAssertEqual(camera.translation, .zero)
        XCTAssertEqual(camera.quaternion, SIMD4(0, 0, 0, 1))
        XCTAssertEqual(coordinates.gravity, SIMD3(0, 1, 0))
        var next = first
        next.columns.3.z -= 1
        XCTAssertEqual(coordinates.camera(transform: next, intrinsics: intrinsics, width: 640, height: 480, timestamp: 1).translation, SIMD3(0, 0, 1))
        let pixel = try camera.project(SIMD3(0, 0, 2))
        XCTAssertEqual(pixel, SIMD2(320, 240))
    }
    func testClockCommitsOnlyAcceptedFrames() throws {
        var clock = CaptureClock()
        XCTAssertEqual(try clock.timestamp(for: 100), 0)
        XCTAssertEqual(try clock.timestamp(for: 101), 0)
        clock.accept(seconds: 101, timestamp: 0)
        XCTAssertEqual(try clock.timestamp(for: 101.125), 125_000_000)
        XCTAssertThrowsError(try clock.timestamp(for: 100))
        XCTAssertThrowsError(try clock.timestamp(for: .nan))
        clock.accept(seconds: 101.125, timestamp: 125_000_000)
        XCTAssertThrowsError(try clock.timestamp(for: 101.125))
    }
    func testRotatedWorldOriginAndMeshPoint() throws {
        var first = simd_float4x4(simd_quatf(angle: .pi / 2, axis: SIMD3(0, 0, 1)))
        first.columns.3 = SIMD4(4, -2, 7, 1)
        let coordinates = CaptureCoordinates(firstCameraTransform: first)
        XCTAssertEqual(coordinates.gravity.x, -1, accuracy: 0.00001)
        XCTAssertEqual(coordinates.gravity.y, 0, accuracy: 0.00001)
        var movement = matrix_identity_float4x4
        movement.columns.3.z = -2
        let intrinsics = simd_float3x3(SIMD3(520, 0, 0), SIMD3(0, 520, 0), SIMD3(319.5, 239.5, 1))
        let next = coordinates.camera(transform: first * movement, intrinsics: intrinsics,
            width: 640, height: 480, timestamp: 1)
        XCTAssertEqual(next.translation.z, 2, accuracy: 0.00001)
        let mesh = coordinates.sceneFromWorld * first * SIMD4<Float>(0.1, 0.2, -2, 1)
        XCTAssertEqual(mesh.x, 0.1, accuracy: 0.00001)
        XCTAssertEqual(mesh.y, -0.2, accuracy: 0.00001)
        XCTAssertEqual(mesh.z, 2, accuracy: 0.00001)
    }
    func testDepthAndConfidenceMapping() {
        XCTAssertEqual(CaptureDepth.sample(meters: 2.5, arConfidence: 0).millimeters, 2500)
        XCTAssertEqual(CaptureDepth.sample(meters: 2.5, arConfidence: 0).confidence, 1)
        XCTAssertEqual(CaptureDepth.sample(meters: 2.5, arConfidence: 1).confidence, 2)
        XCTAssertEqual(CaptureDepth.sample(meters: 2.5, arConfidence: 2).confidence, 3)
        for value: Float in [.nan, .infinity, -1, 0, 70] {
            XCTAssertEqual(CaptureDepth.sample(meters: value, arConfidence: 2).millimeters, 0)
        }
        XCTAssertEqual(CaptureDepth.sample(meters: 2, arConfidence: 3).confidence, 0)
    }
}
#endif
