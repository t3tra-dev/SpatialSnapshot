#if canImport(simd)
import Foundation
import simd
import SpatialSnapshot

public enum CaptureError: Error, LocalizedError {
    case unavailable(String)
    case invalid(String)
    public var errorDescription: String? {
        switch self { case .unavailable(let message), .invalid(let message): return message }
    }
}

/// ARKit (+Y up, -Z forward) to SSPS (+Y down, +Z forward).
/// Both camera bases follow the native sensor raster, independently of UI orientation.
/// Scene +Y is image-down in the first camera; physical down is stored separately in gravity.
public struct CaptureCoordinates: Sendable {
    public static let axisChange = simd_float4x4(diagonal: SIMD4(1, -1, -1, 1))
    public let sceneFromWorld: simd_float4x4
    public let gravity: SIMD3<Double>
    public init(firstCameraTransform: simd_float4x4) {
        sceneFromWorld = simd_inverse(firstCameraTransform * Self.axisChange)
        let down = sceneFromWorld * SIMD4<Float>(0, -1, 0, 0)
        gravity = simd_normalize(SIMD3<Double>(Double(down.x), Double(down.y), Double(down.z)))
    }
    public func camera(transform: simd_float4x4, intrinsics: simd_float3x3,
                       width: UInt32, height: UInt32, timestamp: UInt64, isFirst: Bool = false) -> Camera {
        let pose = sceneFromWorld * transform * Self.axisChange
        var quaternion = simd_normalize(simd_quatf(pose)).vector
        // q and -q represent the same rotation. Choose a stable hemisphere.
        if quaternion.w < 0 { quaternion = -quaternion }
        return Camera(timestampNanoseconds: timestamp, width: width, height: height,
            fx: intrinsics[0, 0], fy: intrinsics[1, 1], cx: intrinsics[2, 0], cy: intrinsics[2, 1],
            translation: isFirst ? .zero : SIMD3(pose.columns.3.x, pose.columns.3.y, pose.columns.3.z),
            quaternion: isFirst ? SIMD4(0, 0, 0, 1) : quaternion)
    }
}

/// The single clock used for accepted RGB, CAMERA and DEPTH samples.
public struct CaptureClock: Sendable {
    public private(set) var origin: Double?
    public private(set) var lastTimestamp: UInt64?
    public init() {}
    public func timestamp(for seconds: Double) throws -> UInt64 {
        guard seconds.isFinite, seconds >= 0 else { throw CaptureError.invalid("ARFrame の時刻が不正です. ") }
        guard let origin else { return 0 }
        let ns = ((seconds - origin) * 1_000_000_000).rounded(.toNearestOrEven)
        guard ns >= 0, ns < Double(Int64.max) else { throw CaptureError.invalid("ARFrame の時刻が逆行しました. ") }
        let timestamp = UInt64(ns)
        if let lastTimestamp, timestamp <= lastTimestamp { throw CaptureError.invalid("ARFrame の時刻が重複・逆行しました. ") }
        return timestamp
    }
    public mutating func accept(seconds: Double, timestamp: UInt64) {
        if origin == nil { origin = seconds }
        lastTimestamp = timestamp
    }
}

public enum CaptureDepth {
    /// ARKit's confidence values are mapped explicitly; they are not wire values.
    public static func sample(meters: Float, arConfidence: UInt8?) -> (millimeters: UInt16, confidence: UInt8) {
        guard meters.isFinite, meters > 0, meters <= 65.535 else { return (0, 0) }
        let mm = (Double(meters) * 1000).rounded(.toNearestOrEven)
        guard mm >= 1, mm <= 65535 else { return (0, 0) }
        let confidence: UInt8
        switch arConfidence {
        case 0: confidence = 1
        case 1: confidence = 2
        case 2: confidence = 3
        case nil: confidence = 1
        default: return (0, 0)
        }
        return (UInt16(mm), confidence)
    }
}
#endif
