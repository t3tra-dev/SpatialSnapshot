#if os(macOS)
import Foundation
import AppKit
import simd
import SpatialSnapshot

enum EditorShading: String, CaseIterable, Identifiable {
    case image = "画像", depth = "深度", wireframe = "ワイヤーフレーム"
    var id: Self { self }
}
enum EditorTool: String, CaseIterable, Identifiable {
    case translate = "移動", rotate = "回転", scale = "スケール"
    var id: Self { self }
    var shortcut: String { switch self { case .translate: "G"; case .rotate: "R"; case .scale: "S" } }
    var icon: String { switch self { case .translate: "move.3d"; case .rotate: "rotate.3d"; case .scale: "scale.3d" } }
}
enum EditorSpace: String, CaseIterable { case scene = "空間", local = "ローカル" }

struct EditorTransform: Codable, Equatable {
    var position = SIMD3<Double>.zero
    var rotationDegrees = SIMD3<Double>.zero
    var scale = SIMD3<Double>(repeating: 1)
    var rotation: simd_quatd {
        let r = rotationDegrees * (.pi / 180)
        return simd_quatd(angle: r.z, axis: SIMD3(0, 0, 1)) *
               simd_quatd(angle: r.y, axis: SIMD3(0, 1, 0)) * simd_quatd(angle: r.x, axis: SIMD3(1, 0, 0))
    }
    var matrix: simd_double4x4 {
        var result = simd_double4x4(rotation)
        result.columns.0 *= scale.x; result.columns.1 *= scale.y; result.columns.2 *= scale.z
        result.columns.3 = SIMD4(position.x, position.y, position.z, 1)
        return result
    }
    var isValid: Bool {
        [position, rotationDegrees, scale].allSatisfy { v in
            (0..<3).allSatisfy { v[$0].isFinite && abs(v[$0]) < Double(Float.greatestFiniteMagnitude) }
        } && (0..<3).allSatisfy { abs(scale[$0]) >= 0.0001 }
    }
    mutating func setRotation(_ quaternion: simd_quatd) {
        let m = simd_double3x3(simd_normalize(quaternion))
        let y = asin(min(1, max(-1, -m.columns.0.z)))
        let x: Double, z: Double
        if abs(cos(y)) > 0.000001 {
            x = atan2(m.columns.1.z, m.columns.2.z)
            z = atan2(m.columns.0.y, m.columns.0.x)
        } else {
            x = atan2(-m.columns.2.y, m.columns.1.y); z = 0
        }
        rotationDegrees = SIMD3(x, y, z) * (180 / .pi)
    }
}

struct EditorAttachment: Codable, Equatable {
    var point: SIMD3<Double>
    var normal: SIMD3<Double>
}
/// Opaque sRGB color, stored independently of AppKit's display color space.
struct EditorColor: Codable, Equatable {
    var red: Double
    var green: Double
    var blue: Double
    static let defaultCube = EditorColor(red: 0.72, green: 0.76, blue: 0.83)
    var isValid: Bool { [red, green, blue].allSatisfy { $0.isFinite && (0...1).contains($0) } }
    var nsColor: NSColor { NSColor(srgbRed: red, green: green, blue: blue, alpha: 1) }
    func matches(_ other: EditorColor) -> Bool {
        // Native color controls round-trip through floating-point color conversions.
        abs(red - other.red) < 0.00001 && abs(green - other.green) < 0.00001 && abs(blue - other.blue) < 0.00001
    }
    static func from(_ color: NSColor) -> EditorColor? {
        guard let rgb = color.usingColorSpace(.sRGB) else { return nil }
        let components = [rgb.redComponent, rgb.greenComponent, rgb.blueComponent]
        guard components.allSatisfy(\.isFinite) else { return nil }
        let clamped = components.map { Double(min(1, max(0, $0))) }
        return EditorColor(red: clamped[0], green: clamped[1], blue: clamped[2])
    }
}
struct EditorObject: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    /// nil denotes the built-in one-meter cube.
    var assetID: UUID?
    var transform = EditorTransform()
    var attachment: EditorAttachment?
    var isVisible = true
    /// Missing in older projects; nil keeps the built-in cube's default color.
    var cubeColor: EditorColor?
    var resolvedCubeColor: EditorColor { cubeColor ?? .defaultCube }
}
struct EditorAssetReference: Codable, Identifiable, Equatable {
    let id: UUID
    var name: String
    var relativePath: String
}
struct EditorProject: Codable, Equatable {
    var version = 1
    var captureID: String
    var objects: [EditorObject] = []
    var assets: [EditorAssetReference] = []
}
enum EditorError: LocalizedError {
    case invalid(String)
    var errorDescription: String? { switch self { case .invalid(let text): text } }
}

struct EditorRay {
    var origin: SIMD3<Double>
    var direction: SIMD3<Double>
}
enum EditorMath {
    static let axes = [SIMD3<Double>(1, 0, 0), SIMD3<Double>(0, 1, 0), SIMD3<Double>(0, 0, 1)]
    static let cubeVertices = [-0.5, 0.5].flatMap { x in
        [-0.5, 0.5].flatMap { y in [-0.5, 0.5].map { z in SIMD3(x, y, z) } }
    }
    static func ray(camera: Camera, pixel: SIMD2<Double>) throws -> EditorRay {
        let origin = SIMD3<Double>(Double(camera.translation.x), Double(camera.translation.y), Double(camera.translation.z))
        return EditorRay(origin: origin, direction: simd_normalize(try camera.unproject(pixel, depthMeters: 1) - origin))
    }
    static func contactPosition(vertices: [SIMD3<Double>], transform: EditorTransform,
                                attachment: EditorAttachment) -> SIMD3<Double> {
        let normal = simd_normalize(attachment.normal)
        let support = vertices.reduce(Double.infinity) {
            min($0, simd_dot(normal, transform.rotation.act($1 * transform.scale)))
        }
        return attachment.point - normal * (support.isFinite ? support : 0)
    }
    static func facingAttachment(hit: SurfaceHit, camera: Camera) -> EditorAttachment {
        let eye = SIMD3<Double>(Double(camera.translation.x), Double(camera.translation.y), Double(camera.translation.z))
        let normal = simd_dot(hit.normal, eye - hit.position) < 0 ? -hit.normal : hit.normal
        return EditorAttachment(point: hit.position, normal: simd_normalize(normal))
    }
    static func planeIntersection(ray: EditorRay, point: SIMD3<Double>, normal: SIMD3<Double>) -> SIMD3<Double>? {
        let denominator = simd_dot(ray.direction, normal)
        guard abs(denominator) > 0.000001 else { return nil }
        let t = simd_dot(point - ray.origin, normal) / denominator
        return t >= 0 ? ray.origin + t * ray.direction : nil
    }
    static func axisParameter(ray: EditorRay, point: SIMD3<Double>, axis: SIMD3<Double>) -> Double? {
        let b = simd_dot(axis, ray.direction), denominator = 1 - b * b
        guard denominator > 0.000001 else { return nil }
        let w = ray.origin - point
        return (simd_dot(axis, w) - b * simd_dot(ray.direction, w)) / denominator
    }
    static func snap(_ value: Double, step: Double, enabled: Bool) -> Double {
        enabled ? (value / step).rounded() * step : value
    }
    /// SceneKit's view coordinates are +Y up / -Z forward; SSPS uses pixel centers.
    static func projection(camera: Camera, near: Double = 0.005, far: Double = 10_000) -> simd_float4x4 {
        let w = Double(camera.width), h = Double(camera.height)
        return simd_float4x4(
            SIMD4(Float(2 * Double(camera.fx) / w), 0, 0, 0),
            SIMD4(0, Float(2 * Double(camera.fy) / h), 0, 0),
            SIMD4(Float(1 - 2 * (Double(camera.cx) + 0.5) / w),
                  Float(2 * (Double(camera.cy) + 0.5) / h - 1), Float(-(far + near) / (far - near)), -1),
            SIMD4(0, 0, Float(-2 * far * near / (far - near)), 0))
    }
}
#endif
