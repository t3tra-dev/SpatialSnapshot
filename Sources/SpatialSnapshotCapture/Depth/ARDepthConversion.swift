#if os(iOS) && canImport(ARKit)
@preconcurrency import ARKit
import SpatialSnapshot

@MainActor
enum ARDepthConversion {
    static func sample(frame: ARFrame, timestamp: UInt64) throws -> DepthSample? {
        guard let depth = frame.sceneDepth else { return nil }
        let buffer = depth.depthMap
        guard CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_DepthFloat32 else {
            throw CaptureError.unavailable("ARKit 深度の画素形式に対応していません. ")
        }
        let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
        let confidence = depth.confidenceMap
        if let confidence {
            guard CVPixelBufferGetWidth(confidence) == width, CVPixelBufferGetHeight(confidence) == height,
                  CVPixelBufferGetPixelFormatType(confidence) == kCVPixelFormatType_OneComponent8 else {
                throw CaptureError.invalid("深度と confidence の画素数・形式が一致しません. ")
            }
        }
        guard CVPixelBufferLockBaseAddress(buffer, .readOnly) == kCVReturnSuccess else {
            throw CaptureError.invalid("深度バッファを読み出せません. ")
        }
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        if let confidence {
            guard CVPixelBufferLockBaseAddress(confidence, .readOnly) == kCVReturnSuccess else {
                throw CaptureError.invalid("confidence バッファを読み出せません. ")
            }
        }
        defer { if let confidence { CVPixelBufferUnlockBaseAddress(confidence, .readOnly) } }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { throw CaptureError.invalid("深度バッファが空です. ") }
        var mm = [UInt16](repeating: 0, count: width * height)
        var levels = [UInt8](repeating: 0, count: width * height)
        for y in 0..<height {
            let row = base.advanced(by: y * CVPixelBufferGetBytesPerRow(buffer)).assumingMemoryBound(to: Float.self)
            let confRow = confidence.flatMap { buffer in
                CVPixelBufferGetBaseAddress(buffer)?.advanced(by: y * CVPixelBufferGetBytesPerRow(buffer)).assumingMemoryBound(to: UInt8.self)
            }
            for x in 0..<width {
                let sample = CaptureDepth.sample(meters: row[x], arConfidence: confRow?[x])
                mm[y * width + x] = sample.millimeters; levels[y * width + x] = sample.confidence
            }
        }
        let camera = frame.camera
        let sx = Float(width) / Float(camera.imageResolution.width)
        let sy = Float(height) / Float(camera.imageResolution.height)
        return try DepthSample(timestampNanoseconds: timestamp, width: UInt32(width), height: UInt32(height),
            fx: camera.intrinsics[0, 0] * sx, fy: camera.intrinsics[1, 1] * sy,
            cx: camera.intrinsics[2, 0] * sx, cy: camera.intrinsics[2, 1] * sy,
            millimeters: mm, confidence: levels)
    }
}
#endif
