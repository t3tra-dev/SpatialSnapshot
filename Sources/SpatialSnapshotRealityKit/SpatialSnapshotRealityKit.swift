#if canImport(RealityKit)
import RealityKit
import SpatialSnapshot
#if os(macOS)
import AppKit
private typealias PlatformColor = NSColor
#else
import UIKit
private typealias PlatformColor = UIColor
#endif

/// An adapter only: the host owns its ARView, scene, camera and interaction.
@MainActor
public enum SpatialSnapshotRealityKit {
    /// The root converts the canonical SSPS axes into RealityKit's axes.
    public static func meshEntity(from scene: SpatialScene, opacity: Float = 0.8) throws -> Entity {
        try makeEntity(meshes: scene.meshes(), occlusion: false, opacity: opacity)
    }
    public static func occlusionEntity(from scene: SpatialScene) throws -> Entity {
        try makeEntity(meshes: scene.meshes(), occlusion: true, opacity: 1)
    }
    /// Draws the supplied quantized chunks, without repartitioning or using ARKit's raw mesh.
    /// The root has the same SSPS-to-RealityKit axis conversion as meshEntity.
    /// iOS 18/macOS 15 use wireframe; earlier systems show translucent triangle surfaces.
    public static func debugMeshEntity(from meshes: [MeshChunk]) throws -> Entity {
        try makeEntity(meshes: meshes, occlusion: false, opacity: 0.22, debug: true)
    }
    public static func realityPosition(_ position: SIMD3<Double>) -> SIMD3<Float> {
        SIMD3(Float(position.x), -Float(position.y), -Float(position.z))
    }
    public static func placementEntity(at hit: SurfaceHit) -> Entity {
        let entity = ModelEntity(mesh: .generateSphere(radius: 0.035),
            materials: [SimpleMaterial(color: .systemOrange, isMetallic: false)])
        entity.position = realityPosition(hit.position)
        return entity
    }
    public static func cameraPath(_ cameras: [Camera]) -> Entity {
        let root = Entity()
        let stride = max(1, cameras.count / 120)
        for index in Swift.stride(from: 0, to: cameras.count, by: stride) {
            let camera = cameras[index]
            let marker = ModelEntity(mesh: .generateSphere(radius: 0.012),
                materials: [UnlitMaterial(color: .systemOrange)])
            marker.position = SIMD3(camera.translation.x, -camera.translation.y, -camera.translation.z)
            root.addChild(marker)
        }
        return root
    }
    private static func makeEntity(meshes: [MeshChunk], occlusion: Bool, opacity: Float, debug: Bool = false) throws -> Entity {
        let root = Entity()
        root.orientation = simd_quatf(angle: .pi, axis: SIMD3(1, 0, 0))
        for chunk in meshes {
            let positions = try chunk.positions().map { SIMD3<Float>(Float($0.x), Float($0.y), Float($0.z)) }
            for classification in Set(chunk.classifications.map(\.rawValue)).sorted() {
                var indices: [UInt32] = []
                for (index, triangle) in chunk.triangles.enumerated() where chunk.classifications[index].rawValue == classification {
                    indices += [UInt32(triangle.x), UInt32(triangle.y), UInt32(triangle.z)]
                }
                guard !indices.isEmpty else { continue }
                var descriptor = MeshDescriptor()
                descriptor.positions = MeshBuffers.Positions(positions)
                descriptor.primitives = .triangles(indices)
                let mesh = try MeshResource.generate(from: [descriptor])
                let material: any Material
                if occlusion { material = OcclusionMaterial() }
                else if debug {
                    var lines = UnlitMaterial(color: color(for: SurfaceClassification(rawValue: classification) ?? .unknown))
                    if #available(iOS 18, macOS 15, *) {
                        lines.triangleFillMode = .lines
                        lines.faceCulling = .none
                        lines.readsDepth = false
                        lines.writesDepth = false
                        lines.blending = .transparent(opacity: .init(floatLiteral: 0.85))
                    } else {
                        lines.blending = .transparent(opacity: .init(floatLiteral: opacity))
                    }
                    material = lines
                }
                else {
                    let color = color(for: SurfaceClassification(rawValue: classification) ?? .unknown)
                    material = SimpleMaterial(color: color.withAlphaComponent(CGFloat(opacity)), roughness: 0.7, isMetallic: false)
                }
                root.addChild(ModelEntity(mesh: mesh, materials: [material]))
            }
        }
        return root
    }
    private static func color(for classification: SurfaceClassification) -> PlatformColor {
        switch classification {
        case .floor: return .systemTeal
        case .wall: return .systemBlue
        case .ceiling: return .systemIndigo
        case .table: return .systemOrange
        case .seat: return .systemPink
        case .window: return .systemCyan
        case .door: return .systemBrown
        default: return .systemGray
        }
    }
}
#endif
