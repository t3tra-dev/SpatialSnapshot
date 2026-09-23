#if os(macOS)
import Foundation
import simd

struct EditorProjectStore {
    let directory: URL
    private var manifestURL: URL { directory.appendingPathComponent("scene.json") }
    static let maximumAssetBytes = 512 * 1024 * 1024

    func load(captureID: String) throws -> EditorProject {
        guard FileManager.default.fileExists(atPath: manifestURL.path) else { return EditorProject(captureID: captureID) }
        let project = try JSONDecoder().decode(EditorProject.self, from: Self.boundedData(manifestURL))
        guard project.version == 1, project.captureID == captureID,
              Set(project.objects.map(\.id)).count == project.objects.count,
              Set(project.assets.map(\.id)).count == project.assets.count,
              project.objects.allSatisfy({ object in
                  object.transform.isValid && (object.cubeColor?.isValid ?? true) &&
                  (object.assetID == nil || project.assets.contains { $0.id == object.assetID }) &&
                  (object.attachment.map { a in
                      a.point.x.isFinite && a.point.y.isFinite && a.point.z.isFinite &&
                      abs(simd_length(a.normal) - 1) < 0.0001
                  } ?? true)
              }) else { throw EditorError.invalid("編集プロジェクトの内容が不正です. ") }
        for asset in project.assets { _ = try assetURL(asset) }
        return project
    }
    func save(_ project: EditorProject) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(project).write(to: manifestURL, options: .atomic)
    }
    func assetURL(_ reference: EditorAssetReference) throws -> URL {
        let url = directory.appendingPathComponent(reference.relativePath).resolvingSymlinksInPath()
        guard url.path.hasPrefix(directory.resolvingSymlinksInPath().path + "/Assets/") else {
            throw EditorError.invalid("モデルの参照先がプロジェクト外を指しています. ")
        }
        return url
    }
    static func boundedData(_ url: URL) throws -> Data {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        guard try file.seekToEnd() <= maximumAssetBytes else {
            throw EditorError.invalid("モデルと関連ファイルは合計 512 MiB 以下にしてください. ")
        }
        try file.seek(toOffset: 0)
        let data = try file.read(upToCount: maximumAssetBytes + 1) ?? Data()
        guard data.count <= maximumAssetBytes else { throw EditorError.invalid("モデルのサイズが大きすぎます. ") }
        return data
    }
    /// Copy the selected model and its local resources into a self-contained project directory.
    /// The folder option grants access to .bin and textures in sandboxed macOS applications.
    func importAsset(from selection: URL) throws -> EditorAssetReference {
        let scope = selection.startAccessingSecurityScopedResource()
        defer { if scope { selection.stopAccessingSecurityScopedResource() } }
        let isDirectory = try selection.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true
        let source: URL
        if isDirectory {
            let models = try FileManager.default.contentsOfDirectory(at: selection,
                includingPropertiesForKeys: nil, options: .skipsHiddenFiles)
                .filter { ["gltf", "glb"].contains($0.pathExtension.lowercased()) }
            guard models.count == 1, let model = models.first else {
                throw EditorError.invalid("フォルダ直下の glTF / GLB を 1 個にするか, 読み込むモデルファイルを選択してください. ")
            }
            source = model
        } else { source = selection }
        guard ["gltf", "glb"].contains(source.pathExtension.lowercased()) else {
            throw EditorError.invalid("glTF 2.0 (.gltf / .glb) を選択してください. ")
        }
        let root = (isDirectory ? selection : source.deletingLastPathComponent()).resolvingSymlinksInPath()
        let id = UUID(), folder = directory.appendingPathComponent("Assets/" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        do {
            let input = try Self.boundedData(source)
            let binary = source.pathExtension.lowercased() == "glb"
            var chunks: [(UInt32, Data)] = []
            var jsonData = input
            if binary {
                func u32(_ offset: Int) -> UInt32 {
                    input.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) }
                }
                guard input.count >= 20, u32(0) == 0x46546c67, u32(4) == 2, Int(u32(8)) == input.count else {
                    throw EditorError.invalid("GLB ヘッダーが不正です. ")
                }
                var cursor = 12
                while cursor < input.count {
                    guard cursor <= input.count - 8 else { throw EditorError.invalid("GLB chunk が途切れています. ") }
                    let length = Int(u32(cursor)), type = u32(cursor + 4)
                    guard length.isMultiple(of: 4), length <= input.count - cursor - 8 else {
                        throw EditorError.invalid("GLB chunk の長さが不正です. ")
                    }
                    chunks.append((type, input.subdata(in: (cursor + 8)..<(cursor + 8 + length))))
                    cursor += 8 + length
                }
                guard chunks.first?.0 == 0x4e4f534a, chunks.filter({ $0.0 == 0x4e4f534a }).count == 1 else {
                    throw EditorError.invalid("GLB JSON chunk が不正です. ")
                }
                jsonData = chunks[0].1
            }
            guard var json = try JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
                  let asset = json["asset"] as? [String: Any], asset["version"] as? String == "2.0" else {
                throw EditorError.invalid("glTF 2.0 のモデルを選択してください. ")
            }
            let supportedExtensions: Set<String> = ["KHR_materials_unlit", "KHR_materials_clearcoat",
                "KHR_materials_emissive_strength", "KHR_texture_transform", "KHR_mesh_quantization",
                "EXT_meshopt_compression", "KHR_lights_punctual"]
            let unsupported = (json["extensionsRequired"] as? [String] ?? []).filter { !supportedExtensions.contains($0) }
            guard unsupported.isEmpty else {
                throw EditorError.invalid("このモデルの必須拡張には対応していません: " + unsupported.joined(separator: ", ") +
                    "\n圧縮なしのメッシュと PNG / JPEG テクスチャで書き出してください. ")
            }
            let nodes = json["nodes"] as? [[String: Any]] ?? []
            let primitives = (json["meshes"] as? [[String: Any]] ?? []).flatMap { $0["primitives"] as? [[String: Any]] ?? [] }
            guard !nodes.contains(where: { $0["skin"] != nil }),
                  !primitives.contains(where: { !($0["targets"] as? [[String: Any]] ?? []).isEmpty }) else {
                throw EditorError.invalid("スキン・モーフを適用した静的メッシュとして glTF を書き出してください. ")
            }
            var total = input.count, resourceIndex = 0
            for key in ["buffers", "images"] {
                guard var items = json[key] as? [[String: Any]] else { continue }
                for i in items.indices {
                    guard let uri = items[i]["uri"] as? String, !uri.hasPrefix("data:") else { continue }
                    guard let parts = URLComponents(string: uri), parts.scheme == nil, parts.host == nil,
                          parts.query == nil, parts.fragment == nil, !uri.hasPrefix("/") else {
                        throw EditorError.invalid("モデルの外部参照はローカルの相対パスにしてください. ")
                    }
                    let resource = source.deletingLastPathComponent()
                        .appendingPathComponent(uri.removingPercentEncoding ?? uri).resolvingSymlinksInPath()
                    guard resource.path.hasPrefix(root.path + "/") else {
                        throw EditorError.invalid("モデルと画像・bin を同じフォルダ内にまとめてください. ")
                    }
                    let data: Data
                    do { data = try Self.boundedData(resource) }
                    catch { throw EditorError.invalid("関連ファイルを読めません. モデルと画像・bin を含むフォルダを選択してください. \n" + error.localizedDescription) }
                    total += data.count
                    guard total <= Self.maximumAssetBytes else { throw EditorError.invalid("モデルと関連ファイルは合計 512 MiB 以下にしてください. ") }
                    let name = "resource-\(resourceIndex)." + (resource.pathExtension.isEmpty ? "bin" : resource.pathExtension)
                    resourceIndex += 1
                    try data.write(to: folder.appendingPathComponent(name), options: .atomic)
                    items[i]["uri"] = name
                }
                json[key] = items
            }
            var output = try JSONSerialization.data(withJSONObject: json, options: [.sortedKeys])
            let filename = binary ? "model.glb" : "model.gltf"
            if binary {
                while !output.count.isMultiple(of: 4) { output.append(0x20) }
                chunks[0].1 = output
                output = Data()
                func append(_ value: UInt32) {
                    var little = value.littleEndian
                    withUnsafeBytes(of: &little) { output.append(contentsOf: $0) }
                }
                append(0x46546c67); append(2); append(UInt32(12 + chunks.reduce(0) { $0 + 8 + $1.1.count }))
                for chunk in chunks { append(UInt32(chunk.1.count)); append(chunk.0); output.append(chunk.1) }
            }
            try output.write(to: folder.appendingPathComponent(filename), options: .atomic)
            return EditorAssetReference(id: id, name: source.deletingPathExtension().lastPathComponent,
                relativePath: "Assets/" + folder.lastPathComponent + "/" + filename)
        } catch {
            try? FileManager.default.removeItem(at: folder)
            throw error
        }
    }
}
#endif
