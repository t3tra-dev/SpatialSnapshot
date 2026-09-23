#if os(macOS)
import SwiftUI
import simd
import SpatialSnapshot

enum EditorGizmoHandle: Equatable {
    case axis(Int), plane(Int), rotation(Int), center
    var axis: Int? { switch self { case .axis(let n), .plane(let n), .rotation(let n): n; case .center: nil } }
}
struct EditorGizmoStroke {
    var handle: EditorGizmoHandle
    var points: [CGPoint]
    var filled = false
}
struct EditorGizmoProjection {
    let origin: SIMD3<Double>
    let axes: [SIMD3<Double>]
    let length: Double
    let strokes: [EditorGizmoStroke]
    let screenAxes: Set<Int>
    let center: CGPoint
    let tool: EditorTool
    let camera: Camera
    let layout: InspectorRasterLayout

    init?(object: EditorObject, tool: EditorTool, space: EditorSpace, camera: Camera, layout: InspectorRasterLayout) {
        self.tool = tool; self.camera = camera; self.layout = layout; origin = object.transform.position
        guard let pixel = try? camera.project(origin) else { return nil }
        center = layout.viewPoint(for: CGPoint(x: pixel.x, y: pixel.y))
        let cameraRotation = simd_quatd(vector: SIMD4(Double(camera.quaternion.x), Double(camera.quaternion.y),
                                                     Double(camera.quaternion.z), Double(camera.quaternion.w)))
        guard let unitPixel = try? camera.project(origin + cameraRotation.act(SIMD3(1, 0, 0))) else { return nil }
        let unit = layout.viewPoint(for: CGPoint(x: unitPixel.x, y: unitPixel.y))
        length = 82 / max(1, hypot(unit.x - center.x, unit.y - center.y))
        axes = (tool == .scale || space == .local) ? EditorMath.axes.map { object.transform.rotation.act($0) } : EditorMath.axes
        let axes = self.axes, length = self.length, origin = self.origin
        func project(_ point: SIMD3<Double>) -> CGPoint? {
            guard let p = try? camera.project(point) else { return nil }
            return layout.viewPoint(for: CGPoint(x: p.x, y: p.y))
        }
        var strokes: [EditorGizmoStroke] = []
        var screenAxes: Set<Int> = []
        for axis in 0..<3 {
            if tool == .rotate {
                let points = (0...96).compactMap { i in
                    let angle = Double(i) * 2 * .pi / 96
                    return project(origin + length * (axes[(axis + 1) % 3] * cos(angle) + axes[(axis + 2) % 3] * sin(angle)))
                }
                if points.count == 97 { strokes.append(.init(handle: .rotation(axis), points: points)) }
            } else {
                if let end = project(origin + axes[axis] * length), hypot(end.x - center.x, end.y - center.y) > 14 {
                    strokes.append(.init(handle: .axis(axis), points: [center, end]))
                } else {
                    // A fixed camera cannot orbit to expose an end-on axis. Provide a dashed
                    // screen handle so all three axes remain editable from the captured view.
                    let offset = [CGPoint(x: 58, y: 58), CGPoint(x: -58, y: 58), CGPoint(x: 58, y: -58)][axis]
                    strokes.append(.init(handle: .axis(axis), points: [center, CGPoint(x: center.x + offset.x, y: center.y + offset.y)]))
                    screenAxes.insert(axis)
                }
            }
            if tool == .translate {
                let u = axes[(axis + 1) % 3], v = axes[(axis + 2) % 3]
                let points = [(0.20, 0.20), (0.40, 0.20), (0.40, 0.40), (0.20, 0.40)].compactMap {
                    project(origin + length * (u * $0.0 + v * $0.1))
                }
                if points.count == 4 { strokes.append(.init(handle: .plane(axis), points: points, filled: true)) }
            }
        }
        if tool != .rotate { strokes.append(.init(handle: .center, points: [center])) }
        self.strokes = strokes
        self.screenAxes = screenAxes
    }
    func point(_ position: SIMD3<Double>) -> CGPoint? {
        guard let pixel = try? camera.project(position) else { return nil }
        return layout.viewPoint(for: CGPoint(x: pixel.x, y: pixel.y))
    }
    func hit(at point: CGPoint) -> EditorGizmoHandle? {
        if tool != .rotate, hypot(point.x - center.x, point.y - center.y) < 9 { return .center }
        for stroke in strokes where stroke.filled {
            if Self.path(stroke.points, closed: true).contains(point) { return stroke.handle }
        }
        var distance: CGFloat = 9, handle: EditorGizmoHandle?
        for stroke in strokes where !stroke.filled && stroke.points.count > 1 {
            for index in 1..<stroke.points.count {
                let candidate = Self.distance(point, segment: (stroke.points[index - 1], stroke.points[index]))
                if candidate < distance { distance = candidate; handle = stroke.handle }
            }
        }
        return handle
    }
    static func path(_ points: [CGPoint], closed: Bool = false) -> Path {
        var path = Path()
        if let first = points.first { path.move(to: first); for p in points.dropFirst() { path.addLine(to: p) } }
        if closed { path.closeSubpath() }
        return path
    }
    static func distance(_ p: CGPoint, segment: (CGPoint, CGPoint)) -> CGFloat {
        let a = segment.0, b = segment.1, dx = b.x - a.x, dy = b.y - a.y
        let t = min(1, max(0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / max(0.000001, dx * dx + dy * dy)))
        return hypot(p.x - a.x - t * dx, p.y - a.y - t * dy)
    }
}

struct EditorGizmoDrag {
    let handle: EditorGizmoHandle
    let tool: EditorTool
    let initial: EditorTransform
    let projection: EditorGizmoProjection
    let start: CGPoint
    let startRay: EditorRay
    private var previousRotationVector: SIMD3<Double>?
    private var rotationAngle = 0.0
    private var rotationSlope = SIMD2<Double>.zero

    init(handle: EditorGizmoHandle, object: EditorObject, projection: EditorGizmoProjection, point: CGPoint, ray: EditorRay) {
        self.handle = handle; tool = projection.tool; initial = object.transform
        self.projection = projection; start = point; startRay = ray
        if case .rotation(let axis) = handle {
            let normal = projection.axes[axis]
            if abs(simd_dot(ray.direction, normal)) > 0.05,
               let hit = EditorMath.planeIntersection(ray: ray, point: projection.origin, normal: normal),
               simd_length(hit - projection.origin) > 0.000001 {
                previousRotationVector = simd_normalize(hit - projection.origin)
            } else {
                // An edge-on ring has no stable ray/plane intersection. Its projected tangent
                // still gives an unambiguous rotation drag, including the fixed first-camera view.
                var best = Double.infinity
                for i in 0..<96 {
                    let angle = Double(i) * 2 * .pi / 96
                    let vector = projection.axes[(axis + 1) % 3] * cos(angle) + projection.axes[(axis + 2) % 3] * sin(angle)
                    guard let p = projection.point(projection.origin + vector * projection.length),
                          let next = projection.point(projection.origin + simd_quatd(angle: 0.01, axis: normal).act(vector) * projection.length) else { continue }
                    let slope = SIMD2(Double(next.x - p.x), Double(next.y - p.y)) / 0.01
                    let distance = hypot(p.x - point.x, p.y - point.y)
                    if distance < best, simd_length(slope) > 5 { best = distance; rotationSlope = slope }
                }
            }
        }
    }
    mutating func update(point: CGPoint, ray: EditorRay, snap: Bool) -> EditorTransform? {
        var value = initial
        let axes = projection.axes, origin = projection.origin
        switch handle {
        case .axis(let axis):
            let delta: Double
            if projection.screenAxes.contains(axis),
               let end = projection.strokes.first(where: { $0.handle == .axis(axis) })?.points.last {
                let direction = simd_normalize(SIMD2(Double(end.x - projection.center.x), Double(end.y - projection.center.y)))
                delta = simd_dot(SIMD2(Double(point.x - start.x), Double(point.y - start.y)), direction) * projection.length / 82
            } else {
                guard let before = EditorMath.axisParameter(ray: startRay, point: origin, axis: axes[axis]),
                      let after = EditorMath.axisParameter(ray: ray, point: origin, axis: axes[axis]) else { return nil }
                delta = after - before
            }
            if tool == .translate {
                value.position += axes[axis] * EditorMath.snap(delta, step: 0.05, enabled: snap)
            } else {
                let factor = max(0.001, EditorMath.snap(1 + delta / projection.length, step: 0.1, enabled: snap))
                value.scale[axis] *= factor
            }
        case .plane(let axis):
            guard let before = EditorMath.planeIntersection(ray: startRay, point: origin, normal: axes[axis]),
                  let after = EditorMath.planeIntersection(ray: ray, point: origin, normal: axes[axis]) else { return nil }
            for i in 0..<3 where i != axis {
                value.position += axes[i] * EditorMath.snap(simd_dot(after - before, axes[i]), step: 0.05, enabled: snap)
            }
        case .rotation(let axis):
            if let previous = previousRotationVector {
                guard let hit = EditorMath.planeIntersection(ray: ray, point: origin, normal: axes[axis]),
                      simd_length(hit - origin) > 0.000001 else { return nil }
                let next = simd_normalize(hit - origin)
                rotationAngle += atan2(simd_dot(axes[axis], simd_cross(previous, next)), simd_dot(previous, next))
                previousRotationVector = next
            } else {
                let delta = SIMD2(Double(point.x - start.x), Double(point.y - start.y))
                guard simd_length_squared(rotationSlope) > 0.00001 else { return nil }
                rotationAngle = simd_dot(delta, rotationSlope) / simd_length_squared(rotationSlope)
            }
            let angle = EditorMath.snap(rotationAngle, step: .pi / 12, enabled: snap)
            value.setRotation(simd_quatd(angle: angle, axis: axes[axis]) * initial.rotation)
        case .center:
            guard tool == .scale else { return nil } // translation center snaps via the C mesh raycast.
            let delta = Double(point.x - start.x - point.y + start.y)
            let factor = max(0.001, EditorMath.snap(exp(delta / 160), step: 0.1, enabled: snap))
            value.scale *= factor
        }
        return value.isValid ? value : nil
    }
}

struct EditorGizmoOverlay: View {
    let projection: EditorGizmoProjection?
    let active: EditorGizmoHandle?
    let bounds: [CGPoint?]
    let placement: CGPoint?
    private let colors: [Color] = [.red, .green, .blue]
    var body: some View {
        Canvas { context, _ in
            if bounds.count == 8 {
                for i in 0..<8 {
                    for bit in [1, 2, 4] where i & bit == 0 {
                        if let a = bounds[i], let b = bounds[i | bit] {
                            context.stroke(EditorGizmoProjection.path([a, b]), with: .color(.orange.opacity(0.7)),
                                           style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                        }
                    }
                }
            }
            if let placement {
                context.stroke(Path(ellipseIn: CGRect(x: placement.x - 6, y: placement.y - 6, width: 12, height: 12)),
                               with: .color(.mint), lineWidth: 1)
            }
            guard let projection else { return }
            for stroke in projection.strokes {
                let color = active == stroke.handle ? Color.yellow : stroke.handle.axis.map { colors[$0] } ?? .white
                if stroke.handle == .center {
                    context.fill(Path(ellipseIn: CGRect(x: projection.center.x - 5, y: projection.center.y - 5, width: 10, height: 10)), with: .color(color))
                } else {
                    let path = EditorGizmoProjection.path(stroke.points, closed: stroke.filled)
                    if stroke.filled { context.fill(path, with: .color(color.opacity(0.2))) }
                    let auxiliary: Bool
                    if case .axis(let axis) = stroke.handle { auxiliary = projection.screenAxes.contains(axis) }
                    else { auxiliary = false }
                    context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: active == stroke.handle ? 3 : 2, dash: auxiliary ? [5, 3] : []))
                    if case .axis(let axis) = stroke.handle, let end = stroke.points.last {
                        if projection.tool == .scale {
                            context.fill(Path(CGRect(x: end.x - 4, y: end.y - 4, width: 8, height: 8)), with: .color(color))
                        } else {
                            let angle = atan2(end.y - projection.center.y, end.x - projection.center.x)
                            let a = CGPoint(x: end.x - 10 * cos(angle - .pi / 6), y: end.y - 10 * sin(angle - .pi / 6))
                            let b = CGPoint(x: end.x - 10 * cos(angle + .pi / 6), y: end.y - 10 * sin(angle + .pi / 6))
                            context.fill(EditorGizmoProjection.path([end, a, b], closed: true), with: .color(color))
                        }
                        context.draw(Text(["X", "Y", "Z"][axis]).font(.caption.bold()).foregroundColor(color),
                                     at: CGPoint(x: end.x + 11, y: end.y - 10))
                    }
                }
            }
        }.allowsHitTesting(false)
    }
}
#endif
