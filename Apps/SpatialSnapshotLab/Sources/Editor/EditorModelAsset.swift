#if os(macOS)
import AppKit
import SceneKit
import SceneKit.ModelIO
import ModelIO
import simd
@preconcurrency import GLTFKit2

@MainActor
final class EditorModelAsset {
    let node: SCNNode
    let vertices: [SIMD3<Double>]
    let dimensions: SIMD3<Double>
    let triangles: [SIMD3<Int>]
    private let minimum: SIMD3<Double>
    private let maximum: SIMD3<Double>
    private struct LoadedAsset: @unchecked Sendable { let value: GLTFAsset }

    init(node: SCNNode, vertices: [SIMD3<Double>], triangles: [SIMD3<Int>]) {
        self.node = node; self.vertices = vertices; self.triangles = triangles
        var minimum = vertices.first ?? .zero, maximum = minimum
        for vertex in vertices { minimum = simd_min(minimum, vertex); maximum = simd_max(maximum, vertex) }
        dimensions = maximum - minimum
        self.minimum = minimum; self.maximum = maximum
    }
    /// Object-local ray direction retains its length, so t remains a scene-space distance
    /// after inverse nonuniform scale. Picking uses the same triangles as rendering.
    func intersection(ray: EditorRay, maximumDistance: Double) -> Double? {
        var enter = 0.0, leave = maximumDistance
        for axis in 0..<3 {
            if abs(ray.direction[axis]) < 1e-12 {
                if ray.origin[axis] < minimum[axis] || ray.origin[axis] > maximum[axis] { return nil }
            } else {
                let a = (minimum[axis] - ray.origin[axis]) / ray.direction[axis]
                let b = (maximum[axis] - ray.origin[axis]) / ray.direction[axis]
                enter = max(enter, min(a, b)); leave = min(leave, max(a, b))
                if enter > leave { return nil }
            }
        }
        var best = maximumDistance, found = false
        for triangle in triangles {
            let a = vertices[triangle.x], e1 = vertices[triangle.y] - a, e2 = vertices[triangle.z] - a
            let p = simd_cross(ray.direction, e2), determinant = simd_dot(e1, p)
            guard abs(determinant) > 1e-12 else { continue }
            let t = ray.origin - a, u = simd_dot(t, p) / determinant
            guard u >= -1e-9, u <= 1 + 1e-9 else { continue }
            let q = simd_cross(t, e1), v = simd_dot(ray.direction, q) / determinant
            guard v >= -1e-9, u + v <= 1 + 1e-9 else { continue }
            let distance = simd_dot(e2, q) / determinant
            if distance >= 0, distance < best { best = distance; found = true }
        }
        return found ? best : nil
    }
    static func cube() -> EditorModelAsset {
        let geometry = SCNBox(width: 1, height: 1, length: 1, chamferRadius: 0)
        let material = SCNMaterial()
        material.diffuse.contents = EditorColor.defaultCube.nsColor
        material.lightingModel = .physicallyBased
        material.roughness.contents = 0.65
        geometry.materials = [material]
        let root = SCNNode(); root.addChildNode(SCNNode(geometry: geometry))
        let faces = [[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1],
                     [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]]
        let triangles = faces.flatMap { [SIMD3($0[0], $0[1], $0[2]), SIMD3($0[0], $0[2], $0[3])] }
        return EditorModelAsset(node: root, vertices: EditorMath.cubeVertices, triangles: triangles)
    }
    private nonisolated static func loadAsset(_ url: URL) async throws -> LoadedAsset {
        try await withCheckedThrowingContinuation { continuation in
            GLTFAsset.load(with: url, options: [:]) { _, status, asset, error, _ in
                if status == .complete, let asset { continuation.resume(returning: LoadedAsset(value: asset)) }
                else if status == .error { continuation.resume(throwing: error ?? EditorError.invalid("glTF を読み込めませんでした. ")) }
            }
        }
    }
    static func load(_ url: URL) async throws -> EditorModelAsset {
        let loaded = try await loadAsset(url)
        let source = GLTFSCNSceneSource(asset: loaded.value)
        guard let scene = source.defaultScene ?? source.scenes.first else { throw EditorError.invalid("glTF に表示可能な scene がありません. ") }
        let root = SCNNode()
        var vertices: [SIMD3<Double>] = []
        var triangles: [SIMD3<Int>] = []
        let change = simd_float4x4(diagonal: SIMD4<Float>(1, -1, -1, 1))
        func visit(_ node: SCNNode, parent: simd_float4x4) throws {
            let transform = parent * node.simdTransform
            if let original = node.geometry {
                let geometry: SCNGeometry
                if original.sources(for: .normal).isEmpty {
                    let mesh = MDLMesh(scnGeometry: original)
                    mesh.addNormals(withAttributeNamed: MDLVertexAttributeNormal, creaseThreshold: 1)
                    geometry = SCNGeometry(mdlMesh: mesh)
                    geometry.materials = original.materials
                } else { geometry = original }
                guard let source = geometry.sources(for: .vertex).first else { throw EditorError.invalid("glTF に頂点がありません. ") }
                guard source.usesFloatComponents, source.componentsPerVector >= 3,
                      [4, 8].contains(source.bytesPerComponent) else {
                    throw EditorError.invalid("この glTF の頂点表現には対応していません. ")
                }
                let copy = SCNNode(geometry: geometry.copy() as? SCNGeometry)
                // Static scene placement: keep node transforms/materials, omit imported cameras,
                // lights and animation controllers. The same base geometry drives contact tests.
                copy.simdTransform = change * transform
                root.addChildNode(copy)
                var referenced: Set<Int> = []
                var localTriangles: [SIMD3<Int>] = []
                for element in geometry.elements {
                    let count: Int
                    switch element.primitiveType {
                    case .triangles: count = element.primitiveCount * 3
                    case .triangleStrip: count = element.primitiveCount + 2
                    default: throw EditorError.invalid("glTF の三角形メッシュを選択してください. ")
                    }
                    guard [1, 2, 4].contains(element.bytesPerIndex), count <= element.data.count / element.bytesPerIndex else {
                        throw EditorError.invalid("glTF のインデックスバッファが不正です. ")
                    }
                    var indices: [Int] = []
                    try element.data.withUnsafeBytes { bytes in
                        for i in 0..<count {
                            let offset = i * element.bytesPerIndex
                            let index: Int
                            switch element.bytesPerIndex {
                            case 1: index = Int(bytes.loadUnaligned(fromByteOffset: offset, as: UInt8.self))
                            case 2: index = Int(bytes.loadUnaligned(fromByteOffset: offset, as: UInt16.self))
                            default: index = Int(bytes.loadUnaligned(fromByteOffset: offset, as: UInt32.self))
                            }
                            guard index < source.vectorCount else { throw EditorError.invalid("glTF の頂点参照が範囲外です. ") }
                            referenced.insert(index)
                            indices.append(index)
                        }
                    }
                    for i in 0..<element.primitiveCount {
                        if element.primitiveType == .triangles {
                            localTriangles.append(SIMD3(indices[i * 3], indices[i * 3 + 1], indices[i * 3 + 2]))
                        } else {
                            localTriangles.append(i.isMultiple(of: 2) ? SIMD3(indices[i], indices[i + 1], indices[i + 2]) :
                                SIMD3(indices[i + 1], indices[i], indices[i + 2]))
                        }
                    }
                }
                var remap: [Int: Int] = [:]
                try source.data.withUnsafeBytes { bytes in
                    for index in referenced.sorted() {
                        let offset = source.dataOffset + index * source.dataStride
                        guard offset >= 0, offset <= bytes.count - 3 * source.bytesPerComponent else {
                            throw EditorError.invalid("glTF の頂点バッファが不正です. ")
                        }
                        func component(_ i: Int) -> Float {
                            let p = offset + i * source.bytesPerComponent
                            return source.bytesPerComponent == 4 ? bytes.loadUnaligned(fromByteOffset: p, as: Float.self) :
                                Float(bytes.loadUnaligned(fromByteOffset: p, as: Double.self))
                        }
                        let point = change * transform * SIMD4(component(0), component(1), component(2), 1)
                        guard point.x.isFinite, point.y.isFinite, point.z.isFinite else { throw EditorError.invalid("glTF に非有限の頂点があります. ") }
                        remap[index] = vertices.count
                        vertices.append(SIMD3(Double(point.x), Double(point.y), Double(point.z)))
                    }
                }
                triangles.append(contentsOf: localTriangles.map { SIMD3(remap[$0.x]!, remap[$0.y]!, remap[$0.z]!) })
            }
            for child in node.childNodes { try visit(child, parent: transform) }
        }
        try visit(scene.rootNode, parent: matrix_identity_float4x4)
        guard let first = vertices.first else { throw EditorError.invalid("glTF にメッシュがありません. ") }
        var minimum = first, maximum = first
        for vertex in vertices { minimum = simd_min(minimum, vertex); maximum = simd_max(maximum, vertex) }
        let center = (minimum + maximum) / 2
        for child in root.childNodes { child.simdPosition -= SIMD3(Float(center.x), Float(center.y), Float(center.z)) }
        return EditorModelAsset(node: root, vertices: vertices.map { $0 - center }, triangles: triangles)
    }
}
#endif
