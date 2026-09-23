#if canImport(CoreGraphics) && canImport(simd)
import CoreGraphics
import simd

/// Viewer policy only. These transforms never change the stored raster or SSPS coordinates.
enum InspectorPresentation {
    /// The first camera is identity, so scene gravity is also its camera-space gravity.
    /// Keep this orientation for the whole video, as a conventional camera recording does.
    static func initialQuarterTurns(gravity: SIMD3<Double>) -> Int {
        guard gravity.x.isFinite, gravity.y.isFinite,
              hypot(gravity.x, gravity.y) >= 0.1 else { return 0 }
        if abs(gravity.x) > abs(gravity.y) { return gravity.x > 0 ? 1 : 3 }
        return gravity.y < 0 ? 2 : 0
    }

    /// Additional rotation for a group whose children already use RealityKit adapter axes.
    /// Make gravity point down while retaining the first camera's horizontal heading.
    static func sceneOrientation(gravity: SIMD3<Double>) -> simd_quatf {
        let down = SIMD3<Float>(Float(gravity.x), -Float(gravity.y), -Float(gravity.z))
        guard down.x.isFinite, down.y.isFinite, down.z.isFinite,
              simd_length_squared(down) > 0.000001 else { return simd_quatf() }
        let up = -simd_normalize(down)
        var right = simd_cross(SIMD3<Float>(0, 0, -1), up)
        // Looking straight up/down has no horizontal forward heading. Preserve image right.
        if simd_length_squared(right) < 0.000001 {
            right = SIMD3<Float>(1, 0, 0) - up.x * up
        }
        right = simd_normalize(right)
        let back = simd_cross(right, up)
        return simd_normalize(simd_quatf(simd_float3x3(right, up, back).transpose))
    }
}

/// One mapping for the image, projected geometry, depth display and inverse tap coordinates.
struct InspectorRasterLayout {
    let quarterTurns: Int
    let sourceSize: CGSize
    let contentRect: CGRect
    private let rasterSize: CGSize
    private let edgeToView: CGAffineTransform
    private let scale: CGFloat

    init(rasterSize: CGSize, viewportSize: CGSize, quarterTurns: Int) {
        self.rasterSize = rasterSize
        self.quarterTurns = ((quarterTurns % 4) + 4) % 4
        let width = rasterSize.width, height = rasterSize.height
        let swapsAxes = self.quarterTurns.isMultiple(of: 2) == false
        let displaySize = swapsAxes ? CGSize(width: height, height: width) : rasterSize
        scale = max(0, min(viewportSize.width / max(1, displaySize.width),
                           viewportSize.height / max(1, displaySize.height)))
        sourceSize = CGSize(width: width * scale, height: height * scale)
        let size = CGSize(width: displaySize.width * scale, height: displaySize.height * scale)
        contentRect = CGRect(x: (viewportSize.width - size.width) / 2,
                             y: (viewportSize.height - size.height) / 2,
                             width: size.width, height: size.height)
        let rotation: CGAffineTransform
        switch self.quarterTurns {
        case 1: rotation = CGAffineTransform(a: 0, b: 1, c: -1, d: 0, tx: height, ty: 0)
        case 2: rotation = CGAffineTransform(a: -1, b: 0, c: 0, d: -1, tx: width, ty: height)
        case 3: rotation = CGAffineTransform(a: 0, b: -1, c: 1, d: 0, tx: 0, ty: width)
        default: rotation = .identity
        }
        edgeToView = CGAffineTransform(a: rotation.a * scale, b: rotation.b * scale,
            c: rotation.c * scale, d: rotation.d * scale,
            tx: contentRect.minX + rotation.tx * scale, ty: contentRect.minY + rotation.ty * scale)
    }

    func viewPoint(for pixel: CGPoint) -> CGPoint {
        // SSPS uses pixel centers; SwiftUI positions the image by its outer edges.
        CGPoint(x: pixel.x + 0.5, y: pixel.y + 0.5).applying(edgeToView)
    }

    func pixel(at point: CGPoint) -> CGPoint? {
        guard scale > 0, contentRect.contains(point) else { return nil }
        let edge = point.applying(edgeToView.inverted())
        return CGPoint(x: min(rasterSize.width - 1, max(0, edge.x - 0.5)),
                       y: min(rasterSize.height - 1, max(0, edge.y - 0.5)))
    }
}
#endif
