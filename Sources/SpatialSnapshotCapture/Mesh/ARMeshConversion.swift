#if os(iOS) && canImport(ARKit)
@preconcurrency import ARKit
import SpatialSnapshot

@MainActor
enum ARMeshConversion {
    static func chunks(anchors: [ARAnchor], coordinates: CaptureCoordinates, maximumFaces: Int?) throws -> [MeshChunk] {
        var triangles: [SceneTriangle] = []
        for anchor in anchors.compactMap({ $0 as? ARMeshAnchor }) {
            let geometry = anchor.geometry
            guard geometry.faces.primitiveType == .triangle, geometry.faces.indexCountPerPrimitive == 3,
                  geometry.vertices.format == .float3,
                  geometry.faces.bytesPerIndex == 2 || geometry.faces.bytesPerIndex == 4 else {
                throw CaptureError.unavailable("ARKit メッシュのバッファ形式に対応していません. ")
            }
            if let maximumFaces {
                guard geometry.faces.count <= maximumFaces - triangles.count else {
                    throw CaptureError.unavailable("メッシュが収録上限 (\(maximumFaces) 面) を超えました. ")
                }
            }
            let transform = coordinates.sceneFromWorld * anchor.transform
            let vertices = geometry.vertices
            func position(_ index: Int) throws -> SIMD3<Double> {
                let offset = vertices.offset + index * vertices.stride
                guard index >= 0, index < vertices.count, offset >= 0,
                      offset <= vertices.buffer.length - 12 else { throw CaptureError.invalid("メッシュ頂点の範囲が不正です. ") }
                let ptr = vertices.buffer.contents().advanced(by: offset)
                let local = SIMD4<Float>(ptr.loadUnaligned(as: Float.self),
                    ptr.advanced(by: 4).loadUnaligned(as: Float.self),
                    ptr.advanced(by: 8).loadUnaligned(as: Float.self), 1)
                let p = transform * local
                return SIMD3(Double(p.x), Double(p.y), Double(p.z))
            }
            for face in 0..<geometry.faces.count {
                var indices: [Int] = []
                for corner in 0..<3 {
                    let offset = (face * 3 + corner) * geometry.faces.bytesPerIndex
                    guard offset <= geometry.faces.buffer.length - geometry.faces.bytesPerIndex else {
                        throw CaptureError.invalid("メッシュ面の範囲が不正です. ")
                    }
                    let ptr = geometry.faces.buffer.contents().advanced(by: offset)
                    indices.append(geometry.faces.bytesPerIndex == 2
                        ? Int(ptr.loadUnaligned(as: UInt16.self)) : Int(ptr.loadUnaligned(as: UInt32.self)))
                }
                let classification: SurfaceClassification
                if let source = geometry.classification {
                    let offset = source.offset + face * source.stride
                    guard offset < source.buffer.length else { throw CaptureError.invalid("メッシュ分類の範囲が不正です. ") }
                    let value = ARMeshClassification(rawValue: Int(source.buffer.contents().advanced(by: offset).load(as: UInt8.self)))
                    switch value {
                    case .wall: classification = .wall
                    case .floor: classification = .floor
                    case .ceiling: classification = .ceiling
                    case .table: classification = .table
                    case .seat: classification = .seat
                    case .window: classification = .window
                    case .door: classification = .door
                    default: classification = .unknown
                    }
                } else { classification = .unknown }
                triangles.append(try SceneTriangle(position(indices[0]), position(indices[1]),
                    position(indices[2]), classification: classification))
            }
        }
        // Partitioning, clipping, quantization and canonical ordering belong to the C core.
        return try MeshChunk.partition(triangles)
    }

    static func updates(previous: [SIMD3<Int32>: MeshChunk], next: [MeshChunk]) -> [GeometryUpdate] {
        let current = Dictionary(uniqueKeysWithValues: next.map { ($0.cell, $0) })
        var result = previous.keys.filter { current[$0] == nil }.map { GeometryUpdate.remove($0) }
        for chunk in next {
            if let old = previous[chunk.cell],
               old.quantizedVertices == chunk.quantizedVertices,
               old.triangles == chunk.triangles, old.classifications == chunk.classifications { continue }
            result.append(.put(chunk))
        }
        return result
    }
}
#endif
