#if canImport(simd)
import Foundation
import simd
import SpatialSnapshot

/// Canonical, quantized capture geometry for host-owned debug rendering.
/// While recording, snapshots are published only after RGB and SSPS both accept the frame.
/// While idle, they contain the mesh prepared by the same conversion used for still capture.
public struct CaptureMeshSnapshot: Identifiable, Sendable {
    public let id: UUID
    public let chunks: [MeshChunk]
    /// Maps the chunks' SSPS scene coordinates directly into the ARSession world.
    public let worldFromScene: simd_float4x4
    public let triangleCount: Int

    public init(chunks: [MeshChunk], coordinates: CaptureCoordinates) {
        id = UUID()
        self.chunks = chunks
        worldFromScene = simd_inverse(coordinates.sceneFromWorld)
        triangleCount = chunks.reduce(0) { $0 + $1.triangles.count }
    }
}
#endif
