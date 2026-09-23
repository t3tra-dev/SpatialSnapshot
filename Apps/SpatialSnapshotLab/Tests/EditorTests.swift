#if os(macOS)
import XCTest
import AppKit
import SceneKit
import Metal
import SwiftUI
import SpatialSnapshot
import SpatialSnapshotAppleMedia

final class EditorTests: XCTestCase {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("SpatialSnapshot-editor-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    @MainActor
    private func capture(in directory: URL) throws -> AppleSpatialAsset {
        let chunks = try MeshChunk.partition([
            SceneTriangle(SIMD3(-3, -3, 2), SIMD3(3, 3, 2), SIMD3(3, -3, 2), classification: .wall),
            SceneTriangle(SIMD3(-3, -3, 2), SIMD3(-3, 3, 2), SIMD3(3, 3, 2), classification: .wall)
        ])
        let writer = try SpatialWriter(kind: .still)
        try writer.append(camera: Camera(width: 640, height: 480, fx: 510, fy: 550, cx: 285.25, cy: 213.75), geometry: chunks.map(GeometryUpdate.put))
        let url = directory.appendingPathComponent("Capture.ssps")
        try writer.finish(durationNanoseconds: 0).write(to: url)
        return try AppleSpatialAsset(url: url)
    }
    func testSurfaceContactPreservesSceneAxesAndSupportsRotationAndScale() {
        for normal in [SIMD3<Double>(0, -1, 0), SIMD3(0, 0, -1), simd_normalize(SIMD3<Double>(1, 2, -3))] {
            for scale in [SIMD3<Double>(0.25, 0.5, 0.75), SIMD3(-0.3, 0.8, 1.2)] {
                var transform = EditorTransform()
                transform.rotationDegrees = SIMD3(23, -37, 51); transform.scale = scale
                let attachment = EditorAttachment(point: SIMD3(1, 2, 3), normal: normal)
                let position = EditorMath.contactPosition(vertices: EditorMath.cubeVertices, transform: transform, attachment: attachment)
                let distances = EditorMath.cubeVertices.map {
                    simd_dot(position + transform.rotation.act($0 * scale) - attachment.point, normal)
                }
                XCTAssertEqual(distances.min()!, 0, accuracy: 0.0000001)
                XCTAssertTrue(distances.allSatisfy { $0 >= -0.0000001 })
                XCTAssertEqual(transform.rotationDegrees, SIMD3(23, -37, 51))
            }
        }
    }
    func testFixedCameraProjectionMatchesSSPSPixelCenters() throws {
        let rotation = simd_quatf(angle: 0.37, axis: simd_normalize(SIMD3<Float>(1, 2, -1)))
        let camera = Camera(width: 640, height: 480, fx: 530, fy: 570, cx: 271.25, cy: 203.5,
                            translation: SIMD3(1, -2, 0.5), quaternion: rotation.vector)
        let projection = EditorMath.projection(camera: camera)
        for pixel in [SIMD2<Double>(0, 0), SIMD2(639, 479), SIMD2(321.25, 201.75)] {
            let point = try camera.unproject(pixel, depthMeters: 2)
            let p = rotation.inverse.act(SIMD3<Float>(Float(point.x), Float(point.y), Float(point.z)) - camera.translation)
            let clip = projection * SIMD4(p.x, -p.y, -p.z, 1)
            let projected = SIMD2((Double(clip.x / clip.w) + 1) * 320 - 0.5,
                                 (1 - Double(clip.y / clip.w)) * 240 - 0.5)
            XCTAssertLessThan(simd_length(projected - pixel), 0.0002)
            XCTAssertLessThan(simd_length(try camera.project(point) - pixel), 0.0001)
        }
    }
    func testGizmoAxisPlaneRotationAndScaleDrags() throws {
        let camera = Camera(width: 640, height: 480, fx: 500, fy: 500, cx: 319.5, cy: 239.5)
        var object = EditorObject(name: "Cube")
        object.transform.position = SIMD3(0, 0, 2)
        let layout = InspectorRasterLayout(rasterSize: CGSize(width: 640, height: 480), viewportSize: CGSize(width: 640, height: 480), quarterTurns: 0)
        func ray(_ point: SIMD3<Double>) throws -> EditorRay { try EditorMath.ray(camera: camera, pixel: camera.project(point)) }
        let translate = try XCTUnwrap(EditorGizmoProjection(object: object, tool: .translate, space: .scene, camera: camera, layout: layout))
        let start = object.transform.position + SIMD3(0.25, 0, 0)
        var axis = EditorGizmoDrag(handle: .axis(0), object: object, projection: translate,
                                  point: try XCTUnwrap(translate.point(start)), ray: try ray(start))
        let end = start + SIMD3(0.18, 0, 0)
        let moved = try XCTUnwrap(axis.update(point: try XCTUnwrap(translate.point(end)), ray: ray(end), snap: true))
        XCTAssertEqual(moved.position.x, 0.2, accuracy: 0.000001)
        XCTAssertEqual(moved.position.z, 2)
        XCTAssertTrue(translate.screenAxes.contains(2))
        let zStart = try XCTUnwrap(translate.strokes.first(where: { $0.handle == .axis(2) })?.points.last)
        let zEnd = CGPoint(x: zStart.x + 58, y: zStart.y - 58)
        var depthDrag = EditorGizmoDrag(handle: .axis(2), object: object, projection: translate, point: zStart, ray: try ray(start))
        let depthMoved = try XCTUnwrap(depthDrag.update(point: zEnd, ray: ray(end), snap: false))
        XCTAssertGreaterThan(depthMoved.position.z, 2.2)
        XCTAssertEqual(depthMoved.position.x, 0)
        var plane = EditorGizmoDrag(handle: .plane(2), object: object, projection: translate,
                                   point: try XCTUnwrap(translate.point(start)), ray: try ray(start))
        let planar = start + SIMD3(0.21, -0.13, 0)
        let movedPlane = try XCTUnwrap(plane.update(point: try XCTUnwrap(translate.point(planar)), ray: ray(planar), snap: true))
        XCTAssertEqual(movedPlane.position.x, 0.2, accuracy: 0.000001)
        XCTAssertEqual(movedPlane.position.y, -0.15, accuracy: 0.000001)
        let rotate = try XCTUnwrap(EditorGizmoProjection(object: object, tool: .rotate, space: .scene, camera: camera, layout: layout))
        let a = object.transform.position + SIMD3(rotate.length, 0, 0)
        let b = object.transform.position + SIMD3(0, rotate.length, 0)
        var ring = EditorGizmoDrag(handle: .rotation(2), object: object, projection: rotate,
                                  point: try XCTUnwrap(rotate.point(a)), ray: try ray(a))
        let rotated = try XCTUnwrap(ring.update(point: try XCTUnwrap(rotate.point(b)), ray: ray(b), snap: false))
        XCTAssertEqual(rotated.rotationDegrees.z, 90, accuracy: 0.00001)
        var edgeOn = EditorGizmoDrag(handle: .rotation(0), object: object, projection: rotate,
                                    point: try XCTUnwrap(rotate.point(b)), ray: try ray(b))
        var screen = try XCTUnwrap(rotate.point(b)); screen.y += 12
        let edgeRay = try EditorMath.ray(camera: camera, pixel: SIMD2(screen.x - 0.5, screen.y - 0.5))
        let edgeRotated = try XCTUnwrap(edgeOn.update(point: screen, ray: edgeRay, snap: false))
        XCTAssertTrue(edgeRotated.isValid)
        XCTAssertGreaterThan(abs(edgeRotated.rotationDegrees.x), 1)
        let scale = try XCTUnwrap(EditorGizmoProjection(object: object, tool: .scale, space: .local, camera: camera, layout: layout))
        var scaling = EditorGizmoDrag(handle: .axis(0), object: object, projection: scale,
                                     point: try XCTUnwrap(scale.point(a)), ray: try ray(a))
        let scaledEnd = a + SIMD3(scale.length, 0, 0)
        let scaled = try XCTUnwrap(scaling.update(point: try XCTUnwrap(scale.point(scaledEnd)), ray: ray(scaledEnd), snap: false))
        XCTAssertEqual(scaled.scale.x, 2, accuracy: 0.00001)
        XCTAssertEqual(scaled.scale.y, 1)
    }

    @MainActor
    func testCubeCRUDUndoPickingAndProjectReopen() async throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let capture = try capture(in: root), scene = try capture.document.scene(), camera = try XCTUnwrap(capture.cameras.first)
        let editor = EditorModel()
        await editor.bind(to: capture, directory: root.appendingPathComponent("Projects"))
        editor.addCube(scene: scene, camera: camera)
        XCTAssertNil(editor.error)
        let object = try XCTUnwrap(editor.selected)
        XCTAssertEqual(object.transform.rotationDegrees, .zero)
        XCTAssertEqual(object.transform.position.z, 1.875, accuracy: 0.00001)
        let ray = try EditorMath.ray(camera: camera, pixel: SIMD2(Double(camera.cx), Double(camera.cy)))
        XCTAssertEqual(editor.pickObject(ray: ray, maximumDistance: 2), object.id)
        XCTAssertNil(editor.pickObject(ray: ray, maximumDistance: 1))
        editor.updateSelected { $0.isVisible = false }
        XCTAssertNil(editor.pickObject(ray: ray, maximumDistance: 2))
        editor.updateSelected { $0.isVisible = true }
        editor.updateSelected({ $0.transform.rotationDegrees = SIMD3(23, 41, 19); $0.transform.scale.x = 0.5 }, maintainContact: true)
        let transformed = try XCTUnwrap(editor.selected)
        let attachment = try XCTUnwrap(transformed.attachment)
        let minimum = EditorMath.cubeVertices.map {
            simd_dot(transformed.transform.position + transformed.transform.rotation.act($0 * transformed.transform.scale) - attachment.point, attachment.normal)
        }.min()!
        XCTAssertEqual(minimum, 0, accuracy: 0.00001)
        editor.duplicateSelected(); XCTAssertEqual(editor.objects.count, 2)
        editor.undo(); XCTAssertEqual(editor.objects.count, 1)
        editor.redo(); XCTAssertEqual(editor.objects.count, 2)
        editor.deleteSelected(); XCTAssertEqual(editor.objects.count, 1)
        editor.undo(); XCTAssertEqual(editor.objects.count, 2)
        editor.save(); XCTAssertFalse(editor.isDirty)
        let reloaded = EditorModel()
        await reloaded.bind(to: capture, directory: root.appendingPathComponent("Projects"))
        XCTAssertNil(reloaded.error)
        XCTAssertEqual(reloaded.objects, editor.objects)
        XCTAssertEqual(try Data(contentsOf: capture.url), capture.spatialData)
    }

    @MainActor
    func testCubeColorsAreIndependentAndSurviveUndoAndProjectReopen() async throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let capture = try capture(in: root), scene = try capture.document.scene(), camera = capture.cameras.first
        let editor = EditorModel(), projects = root.appendingPathComponent("Projects")
        await editor.bind(to: capture, directory: projects)
        editor.addCube(scene: scene, camera: camera)
        let originalID = try XCTUnwrap(editor.selectedID)
        let red = EditorColor(red: 0.9, green: 0.18, blue: 0.2)
        let blue = EditorColor(red: 0.18, green: 0.46, blue: 0.94)
        editor.updateSelected { $0.cubeColor = red }
        let renderer = EditorSceneRenderer(), revision = UUID()
        func update() throws {
            try renderer.update(camera: camera, meshes: { try scene.meshes() }, revision: revision, editor: editor)
        }
        func material(_ id: UUID) throws -> SCNMaterial {
            let node = try XCTUnwrap(renderer.scene.rootNode.childNode(withName: id.uuidString, recursively: true))
            var material: SCNMaterial?
            node.enumerateChildNodes { child, _ in if let value = child.geometry?.firstMaterial { material = value } }
            return try XCTUnwrap(material)
        }
        func color(_ id: UUID) throws -> EditorColor {
            try XCTUnwrap(EditorColor.from(XCTUnwrap(material(id).diffuse.contents as? NSColor)))
        }
        try update()
        XCTAssertTrue(try color(originalID).matches(red))
        editor.duplicateSelected()
        let duplicateID = try XCTUnwrap(editor.selectedID)
        XCTAssertEqual(editor.selected?.cubeColor, red)
        try update()
        XCTAssertFalse(try material(originalID) === material(duplicateID))
        editor.updateSelected { $0.cubeColor = blue }
        for shading in EditorShading.allCases {
            editor.shading = shading
            try update()
            XCTAssertTrue(try color(originalID).matches(red))
            XCTAssertTrue(try color(duplicateID).matches(blue))
        }
        editor.undo(); try update()
        XCTAssertEqual(editor.selected?.cubeColor, red)
        XCTAssertTrue(try color(duplicateID).matches(red))
        editor.redo(); try update()
        XCTAssertEqual(editor.selected?.cubeColor, blue)
        XCTAssertTrue(try color(duplicateID).matches(blue))
        editor.addCube(scene: scene, camera: camera)
        try update()
        XCTAssertTrue(try color(XCTUnwrap(editor.selectedID)).matches(.defaultCube))
        XCTAssertTrue(try color(originalID).matches(red))
        let history = editor.undoCount, objects = editor.objects
        editor.updateSelected { $0.cubeColor = EditorColor(red: .nan, green: 0, blue: 0) }
        XCTAssertNotNil(editor.error)
        XCTAssertEqual(editor.objects, objects)
        XCTAssertEqual(editor.undoCount, history)
        let reloaded = EditorModel()
        await reloaded.bind(to: capture, directory: projects)
        XCTAssertNil(reloaded.error)
        XCTAssertEqual(reloaded.objects, objects) // Color changes auto-save without an explicit save action.
    }

    func testProjectWithoutCubeColorsLoadsAndInvalidColorsAreRejected() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let store = EditorProjectStore(directory: root)
        var project = EditorProject(captureID: "legacy", objects: [EditorObject(name: "Cube")])
        try store.save(project)
        let json = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("scene.json"))) as? [String: Any])
        let objects = try XCTUnwrap(json["objects"] as? [[String: Any]])
        XCTAssertNil(objects.first?["cubeColor"])
        let legacy = try store.load(captureID: "legacy")
        XCTAssertEqual(legacy.objects.first?.resolvedCubeColor, .defaultCube)
        for color in [EditorColor(red: -0.1, green: 0, blue: 0),
                      EditorColor(red: 0, green: 1.1, blue: 0), EditorColor(red: 0, green: 0, blue: 2)] {
            project.objects[0].cubeColor = color
            try store.save(project)
            XCTAssertThrowsError(try store.load(captureID: "legacy"))
        }
    }

    private func makeGLTF(in root: URL, binary: Bool) throws -> URL {
        let folder = root.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        // The unused fourth vertex must not affect the rendered model's bounds/contact point.
        let positions: [Float] = [-0.2, -0.1, 0, 0.2, -0.1, 0, 0, 0.3, 0, 20, 30, 40]
        var buffer = positions.withUnsafeBytes { Data($0) }
        let normals: [Float] = [0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]
        buffer.append(normals.withUnsafeBytes { Data($0) })
        let uvs: [Float] = [0, 0, 1, 0, 0.5, 1, 0, 0]
        buffer.append(uvs.withUnsafeBytes { Data($0) })
        let indices: [UInt16] = [0, 1, 2]
        buffer.append(indices.withUnsafeBytes { Data($0) })
        var json: [String: Any] = [
            "asset": ["version": "2.0"], "scene": 0, "scenes": [["nodes": [0]]],
            "nodes": [["children": [1], "scale": [1.5, 2, 1]], ["mesh": 0, "translation": [0.1, 0.2, 0.3]]],
            "meshes": [["primitives": [["attributes": binary ? ["POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2] : ["POSITION": 0, "TEXCOORD_0": 2], "indices": 3, "material": 0]]]],
            "materials": [["pbrMetallicRoughness": ["baseColorTexture": ["index": 0], "metallicFactor": 0]]],
            "images": [["uri": "texture.png"]], "textures": [["source": 0]],
            "buffers": [["byteLength": buffer.count]],
            "bufferViews": [["buffer": 0, "byteOffset": 0, "byteLength": 48], ["buffer": 0, "byteOffset": 48, "byteLength": 48],
                            ["buffer": 0, "byteOffset": 96, "byteLength": 32], ["buffer": 0, "byteOffset": 128, "byteLength": 6]],
            "accessors": [["bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-0.2, -0.1, 0], "max": [20, 30, 40]],
                          ["bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"],
                          ["bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"],
                          ["bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"]]
        ]
        let texture = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 2, pixelsHigh: 2, bitsPerSample: 8,
            samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 8, bitsPerPixel: 32)!
        for y in 0..<2 { for x in 0..<2 {
            texture.setColor((x + y).isMultiple(of: 2) ? NSColor(deviceRed: 1, green: 0, blue: 0, alpha: 1) :
                NSColor(deviceRed: 0, green: 0, blue: 1, alpha: 1), atX: x, y: y)
        } }
        try texture.representation(using: .png, properties: [:])!.write(to: folder.appendingPathComponent("texture.png"))
        let url = folder.appendingPathComponent(binary ? "Triangle.glb" : "Triangle.gltf")
        if binary {
            json.removeValue(forKey: "scene") // A default-scene index is optional in glTF.
            var jsonData = try JSONSerialization.data(withJSONObject: json)
            while !jsonData.count.isMultiple(of: 4) { jsonData.append(0x20) }
            while !buffer.count.isMultiple(of: 4) { buffer.append(0) }
            var output = Data()
            func append(_ value: UInt32) { var little = value.littleEndian; withUnsafeBytes(of: &little) { output.append(contentsOf: $0) } }
            append(0x46546c67); append(2); append(UInt32(12 + 8 + jsonData.count + 8 + buffer.count))
            append(UInt32(jsonData.count)); append(0x4e4f534a); output.append(jsonData)
            append(UInt32(buffer.count)); append(0x004e4942); output.append(buffer)
            try output.write(to: url)
        } else {
            json["buffers"] = [["uri": "mesh%20data.bin", "byteLength": buffer.count]]
            try buffer.write(to: folder.appendingPathComponent("mesh data.bin"))
            try JSONSerialization.data(withJSONObject: json).write(to: url)
        }
        return url
    }
    @MainActor
    func testGLTFAndGLBImportCopiesResourcesAndSurvivesReopen() async throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let capture = try capture(in: root), scene = try capture.document.scene(), camera = capture.cameras.first
        let editor = EditorModel(), projects = root.appendingPathComponent("Projects")
        await editor.bind(to: capture, directory: projects)
        for binary in [false, true] {
            let original = try makeGLTF(in: root, binary: binary)
            await editor.importGLTF(binary ? original : original.deletingLastPathComponent(), scene: scene, camera: camera)
            XCTAssertNil(editor.error)
            let object = try XCTUnwrap(editor.selected)
            XCTAssertNotNil(object.assetID)
            let asset = try XCTUnwrap(editor.asset(for: object))
            XCTAssertEqual(asset.vertices.count, 3)
            XCTAssertEqual(asset.dimensions.x, 0.6, accuracy: 0.00001)
            XCTAssertEqual(asset.dimensions.y, 0.8, accuracy: 0.00001)
            XCTAssertGreaterThan(asset.node.childNodes.count, 0)
            let geometry = try XCTUnwrap(asset.node.childNodes.first?.geometry)
            XCTAssertFalse(geometry.sources(for: .normal).isEmpty)
            XCTAssertFalse(geometry.sources(for: .texcoord).isEmpty)
            XCTAssertNotNil(geometry.firstMaterial?.diffuse.contents)
            let ray = try EditorMath.ray(camera: XCTUnwrap(camera), pixel: SIMD2(Double(camera!.cx), Double(camera!.cy)))
            XCTAssertNotNil(editor.pickObject(ray: ray, maximumDistance: 2.01))
            try FileManager.default.removeItem(at: original.deletingLastPathComponent())
        }
        let reloaded = EditorModel(); await reloaded.bind(to: capture, directory: projects)
        XCTAssertNil(reloaded.error)
        XCTAssertEqual(reloaded.objects, editor.objects)
        XCTAssertEqual(reloaded.objects.count, 2)
    }
    func testProjectImporterRejectsExternalPathsAndMalformedGLB() throws {
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let store = EditorProjectStore(directory: root.appendingPathComponent("Project"))
        let source = try makeGLTF(in: root, binary: false)
        var json = try JSONSerialization.jsonObject(with: Data(contentsOf: source)) as! [String: Any]
        json["buffers"] = [["uri": "../outside.bin", "byteLength": 4]]
        try JSONSerialization.data(withJSONObject: json).write(to: source)
        XCTAssertThrowsError(try store.importAsset(from: source))
        json["buffers"] = []
        json["extensionsRequired"] = ["KHR_draco_mesh_compression"]
        try JSONSerialization.data(withJSONObject: json).write(to: source)
        XCTAssertThrowsError(try store.importAsset(from: source))
        let malformed = root.appendingPathComponent("Malformed.glb")
        try Data(repeating: 0, count: 20).write(to: malformed)
        XCTAssertThrowsError(try store.importAsset(from: malformed))
        XCTAssertThrowsError(try store.assetURL(EditorAssetReference(id: UUID(), name: "external", relativePath: "../outside.gltf")))
    }
    @MainActor
    func testRendererUsesFixedCameraAndCapturedMeshOcclusion() async throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Requires Apple rendering services; enable with the Apple media tests.")
        }
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let capture = try capture(in: root), scene = try capture.document.scene(), camera = try XCTUnwrap(capture.cameras.first)
        let editor = EditorModel(); await editor.bind(to: capture, directory: root.appendingPathComponent("Projects"))
        editor.addCube(scene: scene, camera: camera)
        editor.updateSelected {
            $0.transform.position.x = 0.35; $0.transform.position.y = -0.2; $0.attachment = nil
            $0.cubeColor = EditorColor(red: 0.9, green: 0.1, blue: 0.1)
        }
        let renderer = EditorSceneRenderer(), revision = UUID()
        try renderer.update(camera: camera, meshes: { try scene.meshes() }, revision: revision, editor: editor)
        let offscreen = SCNRenderer(device: MTLCreateSystemDefaultDevice(), options: nil)
        offscreen.scene = renderer.scene; offscreen.pointOfView = renderer.cameraNode
        func snapshot() throws -> NSBitmapImageRep {
            let image = offscreen.snapshot(atTime: 0, with: CGSize(width: 640, height: 480), antialiasingMode: .multisampling4X)
            return try XCTUnwrap(NSBitmapImageRep(data: image.tiffRepresentation!))
        }
        let pixel = try camera.project(editor.selected!.transform.position)
        let visible = try snapshot()
        let color = try XCTUnwrap(visible.colorAt(x: Int(pixel.x.rounded()), y: Int(pixel.y.rounded()))?.usingColorSpace(.deviceRGB))
        XCTAssertGreaterThan(color.redComponent + color.greenComponent + color.blueComponent, 0.2)
        XCTAssertLessThan(color.redComponent + color.greenComponent + color.blueComponent, 2.9)
        XCTAssertGreaterThan(color.redComponent, color.blueComponent + 0.1)
        if let output = ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_EDITOR_RENDER"] {
            try visible.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: output))
        }
        editor.updateSelected { $0.cubeColor = EditorColor(red: 0.1, green: 0.1, blue: 0.9) }
        try renderer.update(camera: camera, meshes: { try scene.meshes() }, revision: revision, editor: editor)
        let recolored = try snapshot()
        let blue = try XCTUnwrap(recolored.colorAt(x: Int(pixel.x.rounded()), y: Int(pixel.y.rounded()))?.usingColorSpace(.deviceRGB))
        XCTAssertGreaterThan(blue.blueComponent, blue.redComponent + 0.1)
        editor.updateSelected { $0.transform.position.z = 2.5 }
        try renderer.update(camera: camera, meshes: { try scene.meshes() }, revision: revision, editor: editor)
        let hiddenPixel = try camera.project(editor.selected!.transform.position)
        let hidden = try snapshot()
        let background = try XCTUnwrap(hidden.colorAt(x: Int(hiddenPixel.x.rounded()), y: Int(hiddenPixel.y.rounded()))?.usingColorSpace(.deviceRGB))
        XCTAssertLessThan(background.redComponent + background.greenComponent + background.blueComponent, 0.05)
        editor.shading = .wireframe
        try renderer.update(camera: camera, meshes: { try scene.meshes() }, revision: revision, editor: editor)
        let wire = try snapshot()
        let edge = try XCTUnwrap(wire.colorAt(x: Int(camera.cx.rounded()), y: Int(camera.cy.rounded()))?.usingColorSpace(.deviceRGB))
        XCTAssertGreaterThan(edge.redComponent + edge.greenComponent + edge.blueComponent, 0.1)
    }
    @MainActor
    func testHighResolutionImageFitsViewportAfterRotationAndResize() async throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Requires AppKit / ImageIO; enable with the Apple media tests.")
        }
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let width = 1920, height = 1440
        let context = try XCTUnwrap(CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
            bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        context.setFillColor(CGColor(gray: 0.8, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        for (x, y, color) in [
            (0, 0, CGColor(red: 0.9, green: 0.1, blue: 0.1, alpha: 1)),
            (width * 3 / 4, 0, CGColor(red: 0.1, green: 0.9, blue: 0.1, alpha: 1)),
            (0, height * 3 / 4, CGColor(red: 0.1, green: 0.1, blue: 0.9, alpha: 1)),
            (width * 3 / 4, height * 3 / 4, CGColor(red: 0.9, green: 0.9, blue: 0.1, alpha: 1))
        ] {
            context.setFillColor(color)
            context.fill(CGRect(x: x, y: y, width: width / 4, height: height / 4))
        }
        let image = try XCTUnwrap(context.makeImage())
        let writer = try SpatialWriter(kind: .still)
        try writer.append(camera: Camera(width: UInt32(width), height: UInt32(height),
            fx: 1560, fy: 1560, cx: 959.5, cy: 719.5))
        let url = root.appendingPathComponent("HighResolution.heic")
        try SpatialHEIFEncoder.encode(image: image, spatialData: writer.finish(durationNanoseconds: 0)).write(to: url)
        let lab = LabModel(libraryDirectory: root)
        await lab.open(url)
        XCTAssertNil(lab.error)
        let editor = EditorModel()
        await editor.bind(to: lab.asset, directory: try lab.editorProjectDirectory())
        let host = NSHostingView(rootView: EditorViewport(lab: lab, editor: editor).preferredColorScheme(.dark))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 480),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = host
        defer { window.contentView = nil }
        // Test-owned offscreen window; exercise the actual native raster and scene views.
        func descendants<T: NSView>(_ view: NSView, of type: T.Type) -> [T] {
            ((view as? T).map { [$0] } ?? []) + view.subviews.flatMap { descendants($0, of: type) }
        }
        for (size, turns) in [(CGSize(width: 640, height: 480), 0), (CGSize(width: 640, height: 480), 1),
                              (CGSize(width: 540, height: 720), 1), (CGSize(width: 900, height: 420), 3)] {
            lab.rasterQuarterTurns = turns
            window.setContentSize(size)
            for _ in 0..<3 {
                try await Task.sleep(for: .milliseconds(30))
                host.layoutSubtreeIfNeeded(); host.displayIfNeeded()
            }
            let layout = InspectorRasterLayout(rasterSize: CGSize(width: width, height: height),
                                               viewportSize: host.bounds.size, quarterTurns: turns)
            let raster = try XCTUnwrap(descendants(host, of: NSImageView.self).first { $0.image != nil })
            let scene = try XCTUnwrap(descendants(host, of: EditorSceneContainer.self).first)
            XCTAssertEqual(raster.bounds.width, layout.sourceSize.width, accuracy: 0.01)
            XCTAssertEqual(raster.bounds.height, layout.sourceSize.height, accuracy: 0.01)
            XCTAssertEqual(scene.sceneView.bounds.width, layout.sourceSize.width, accuracy: 0.01)
            XCTAssertEqual(scene.sceneView.bounds.height, layout.sourceSize.height, accuracy: 0.01)
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            // All four corner markers must remain visible at their projected positions.
            // Compare channel dominance because encoding and display color management
            // can change the exact values. Channel indices are R = 0, G = 1, B = 2.
            for (pixel, bright, dark) in [
                (CGPoint(x: 192, y: 144), [2], [0, 1]),
                (CGPoint(x: 1728, y: 144), [0, 1], [2]),
                (CGPoint(x: 192, y: 1296), [0], [1, 2]),
                (CGPoint(x: 1728, y: 1296), [1], [0, 2])
            ] {
                let point = layout.viewPoint(for: pixel)
                let actual = try XCTUnwrap(bitmap.colorAt(x: Int(point.x * CGFloat(bitmap.pixelsWide) / host.bounds.width),
                    y: Int(point.y * CGFloat(bitmap.pixelsHigh) / host.bounds.height))?.usingColorSpace(.sRGB))
                let channels = [actual.redComponent, actual.greenComponent, actual.blueComponent]
                let contrast = try XCTUnwrap(bright.map { channels[$0] }.min()) - XCTUnwrap(dark.map { channels[$0] }.max())
                XCTAssertGreaterThan(contrast, 0.25, "Missing corner at \(pixel), viewport \(size), rotation \(turns)")
                let roundTrip = try XCTUnwrap(layout.pixel(at: point))
                XCTAssertEqual(roundTrip.x, pixel.x, accuracy: 0.001)
                XCTAssertEqual(roundTrip.y, pixel.y, accuracy: 0.001)
            }
            if let folder = ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_EDITOR_UI_RENDER"] {
                try bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: folder)
                    .appendingPathComponent("editor-fit-\(Int(size.width))x\(Int(size.height))-\(turns).png"))
            }
        }
    }
    @MainActor
    func testEditorLayoutPreview() async throws {
        guard let folder = ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_EDITOR_UI_RENDER"] else {
            throw XCTSkip("Set SPATIALSNAPSHOT_EDITOR_UI_RENDER to render the Editor layout without opening the app.")
        }
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let lab = LabModel(libraryDirectory: root)
        let room = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("Resources/Room.heic")
        await lab.open(room)
        XCTAssertNil(lab.error)
        XCTAssertNotNil(lab.image)
        let editor = EditorModel(); await editor.bind(to: lab.asset, directory: try lab.editorProjectDirectory())
        editor.addCube(scene: lab.scene, camera: lab.camera)
        XCTAssertNotNil(editor.selected)
        let host = NSHostingView(rootView: EditorView(lab: lab, editor: editor).tint(.mint).preferredColorScheme(.dark))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1020, height: 780), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = host
        host.frame = NSRect(x: 0, y: 0, width: 1020, height: 780)
        host.layoutSubtreeIfNeeded(); host.displayIfNeeded()
        try await Task.sleep(for: .milliseconds(250))
        // This test-created window remains offscreen; it does not control the running app.
        for shading in EditorShading.allCases + [.image] {
            editor.shading = shading
            try await Task.sleep(for: .milliseconds(100))
            host.layoutSubtreeIfNeeded(); host.displayIfNeeded()
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            if shading != .wireframe {
                let x = Int(200 * Double(bitmap.pixelsWide) / host.bounds.width)
                let y = Int(240 * Double(bitmap.pixelsHigh) / host.bounds.height)
                let pixel = try XCTUnwrap(bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB))
                if shading == .image { XCTAssertGreaterThan(pixel.blueComponent - pixel.redComponent, 0.05) }
                else { XCTAssertGreaterThan(pixel.greenComponent - pixel.redComponent, 0.05) }
            }
            let name = shading == .image ? "image" : shading == .depth ? "depth" : "wireframe"
            try bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: folder).appendingPathComponent("editor-\(name).png"))
        }
        window.contentView = nil
    }
}
#endif
