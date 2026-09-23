#if canImport(SwiftUI)
import SwiftUI
import RealityKit
import SpatialSnapshot
import SpatialSnapshotRealityKit

struct SpatialSceneView: View {
    @ObservedObject var model: LabModel
    @State private var azimuth: Double = 0.4
    @State private var elevation: Double = 0.35
    @State private var distance: Double = 4
    @State private var dragOrigin: SIMD2<Double>?
    var body: some View {
        VStack(spacing: 0) {
            SceneSurface(scene: model.scene, cameras: model.asset?.cameras ?? [], hit: model.hit,
                         gravity: model.asset?.document.gravity ?? SIMD3(0, 1, 0),
                         revision: model.revision, azimuth: azimuth, elevation: elevation, distance: distance,
                         onError: { model.report($0) })
                .contentShape(Rectangle())
                .gesture(DragGesture().onChanged { value in
                    if dragOrigin == nil { dragOrigin = SIMD2(azimuth, elevation) }
                    azimuth = dragOrigin!.x - value.translation.width * 0.008
                    elevation = min(1.3, max(-0.5, dragOrigin!.y + value.translation.height * 0.008))
                }.onEnded { _ in dragOrigin = nil })
                .overlay(alignment: .topLeading) {
                    HStack(spacing: 12) {
                        Label("床", systemImage: "square.fill").foregroundStyle(.teal)
                        Label("壁", systemImage: "square.fill").foregroundStyle(.blue)
                        Label("机・軌跡", systemImage: "square.fill").foregroundStyle(.orange)
                    }.font(.caption2).padding(12).background(.ultraThinMaterial, in: Capsule()).padding()
                }
            HStack {
                Image(systemName: "minus.magnifyingglass")
                Slider(value: $distance, in: 1...12).accessibilityLabel("3D カメラの距離")
                Image(systemName: "plus.magnifyingglass")
                Button("視点を戻す") { azimuth = 0.4; elevation = 0.35; distance = 4 }
            }.font(.caption).padding(12)
        }
    }
}

@MainActor
private final class SceneCoordinator {
    let anchor = AnchorEntity(world: .zero)
    let camera = PerspectiveCamera()
    var content: Entity?
    var revision: UUID?
    var center = SIMD3<Float>(0, 0, -1.5)
    func makeView() -> ARView {
        #if os(macOS)
        let view = ARView(frame: .zero)
        #else
        let view = ARView(frame: .zero, cameraMode: .nonAR, automaticallyConfigureSession: false)
        #endif
        view.environment.background = .color(.init(white: 0.035, alpha: 1))
        view.scene.addAnchor(anchor)
        anchor.addChild(camera)
        let light = DirectionalLight()
        light.light.intensity = 2500
        light.look(at: SIMD3(0, -1, -2), from: SIMD3(2, 5, 2), relativeTo: nil)
        anchor.addChild(light)
        return view
    }
    func update(_ value: SceneSurface) {
        if revision != value.revision {
            revision = value.revision
            content?.removeFromParent()
            let root = Entity()
            root.orientation = InspectorPresentation.sceneOrientation(gravity: value.gravity)
            center = root.orientation.act(SIMD3(0, 0, -1.5))
            do {
                if let scene = value.scene {
                    root.addChild(try SpatialSnapshotRealityKit.meshEntity(from: scene, opacity: 1))
                    let positions = try scene.meshes().flatMap { try $0.positions() }.map {
                        root.orientation.act(SpatialSnapshotRealityKit.realityPosition($0))
                    }
                    if let first = positions.first {
                        var minimum = first, maximum = first
                        for point in positions { minimum = simd_min(minimum, point); maximum = simd_max(maximum, point) }
                        center = (minimum + maximum) / 2
                    }
                }
                root.addChild(SpatialSnapshotRealityKit.cameraPath(value.cameras))
                if let hit = value.hit { root.addChild(SpatialSnapshotRealityKit.placementEntity(at: hit)) }
            } catch {
                // The callback publishes an alert; do not invoke it inside a view update.
                Task { @MainActor [weak self] in
                    guard self?.revision == value.revision else { return }
                    value.onError(error)
                }
            }
            anchor.addChild(root); content = root
        }
        let r = Float(value.distance), a = Float(value.azimuth), e = Float(value.elevation)
        let position = center + SIMD3(r * sin(a) * cos(e), r * sin(e), r * cos(a) * cos(e))
        camera.look(at: center, from: position, relativeTo: nil)
    }
}

private struct SceneSurface {
    let scene: SpatialScene?
    let cameras: [Camera]
    let hit: SurfaceHit?
    let gravity: SIMD3<Double>
    let revision: UUID
    let azimuth: Double
    let elevation: Double
    let distance: Double
    let onError: @MainActor (Error) -> Void
}
#if os(macOS)
extension SceneSurface: NSViewRepresentable {
    func makeCoordinator() -> SceneCoordinator { SceneCoordinator() }
    func makeNSView(context: Context) -> ARView { context.coordinator.makeView() }
    func updateNSView(_ view: ARView, context: Context) { context.coordinator.update(self) }
}
#else
extension SceneSurface: UIViewRepresentable {
    func makeCoordinator() -> SceneCoordinator { SceneCoordinator() }
    func makeUIView(context: Context) -> ARView { context.coordinator.makeView() }
    func updateUIView(_ view: ARView, context: Context) { context.coordinator.update(self) }
}
#endif

#endif
