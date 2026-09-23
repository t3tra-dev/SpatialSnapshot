#if os(macOS)
import SwiftUI
import SceneKit
import SpatialSnapshot

@MainActor
final class EditorSceneRenderer {
    let scene = SCNScene()
    let cameraNode = SCNNode()
    private let environment = SCNNode()
    private let wireframe = SCNNode()
    private let content = SCNNode()
    private var sceneRevision: UUID?
    private var nodes: [UUID: SCNNode] = [:]
    private var nodeAssets: [UUID: UUID] = [:]

    init() {
        scene.rootNode.addChildNode(environment); scene.rootNode.addChildNode(wireframe)
        scene.rootNode.addChildNode(content); scene.rootNode.addChildNode(cameraNode)
        cameraNode.camera = SCNCamera()
        cameraNode.camera?.automaticallyAdjustsZRange = false
        cameraNode.camera?.zNear = 0.005; cameraNode.camera?.zFar = 10_000
        cameraNode.camera?.wantsHDR = true
        cameraNode.camera?.wantsExposureAdaptation = false
        let ambient = SCNNode(); ambient.light = SCNLight(); ambient.light?.type = .ambient
        ambient.light?.intensity = 2; scene.rootNode.addChildNode(ambient)
        let key = SCNNode(); key.light = SCNLight(); key.light?.type = .omni
        key.light?.intensity = 6; key.position = SCNVector3(-1, 1, 0)
        cameraNode.addChildNode(key)
    }
    func update(camera: Camera?, meshes: () throws -> [MeshChunk], revision: UUID,
                editor: EditorModel) throws {
        SCNTransaction.begin(); SCNTransaction.disableActions = true
        defer { SCNTransaction.commit() }
        if let camera {
            var pose = simd_float4x4(simd_quatf(vector: camera.quaternion))
            pose.columns.3 = SIMD4(camera.translation.x, camera.translation.y, camera.translation.z, 1)
            cameraNode.simdTransform = pose * simd_float4x4(diagonal: SIMD4(1, -1, -1, 1))
            cameraNode.camera?.projectionTransform = SCNMatrix4(EditorMath.projection(camera: camera))
        }
        if sceneRevision != revision {
            sceneRevision = revision
            environment.childNodes.forEach { $0.removeFromParentNode() }
            wireframe.childNodes.forEach { $0.removeFromParentNode() }
            let depth = SCNMaterial()
            depth.colorBufferWriteMask = []; depth.writesToDepthBuffer = true
            depth.readsFromDepthBuffer = true; depth.isDoubleSided = true
            let lines = SCNMaterial(); lines.lightingModel = .constant
            lines.diffuse.contents = NSColor(calibratedWhite: 0.64, alpha: 1)
            lines.fillMode = .lines; lines.isDoubleSided = true
            lines.writesToDepthBuffer = false; lines.readsFromDepthBuffer = false
            for chunk in try meshes() {
                let vertices = try chunk.positions().map { SCNVector3($0.x, $0.y, $0.z) }
                let indices = chunk.triangles.flatMap { [UInt32($0.x), UInt32($0.y), UInt32($0.z)] }
                let source = SCNGeometrySource(vertices: vertices)
                let element = SCNGeometryElement(indices: indices, primitiveType: .triangles)
                let geometry = SCNGeometry(sources: [source], elements: [element]); geometry.materials = [depth]
                let node = SCNNode(geometry: geometry); node.renderingOrder = -100
                environment.addChildNode(node)
                let wire = SCNGeometry(sources: [source], elements: [element]); wire.materials = [lines]
                let outline = SCNNode(geometry: wire); outline.renderingOrder = -50
                wireframe.addChildNode(outline)
            }
        }
        wireframe.isHidden = editor.shading != .wireframe
        let ids = Set(editor.objects.map(\.id))
        for id in Array(nodes.keys) where !ids.contains(id) {
            nodes.removeValue(forKey: id)?.removeFromParentNode(); nodeAssets.removeValue(forKey: id)
        }
        for object in editor.objects {
            if nodes[object.id] == nil || nodeAssets[object.id] != object.assetID {
                nodes[object.id]?.removeFromParentNode()
                guard let asset = editor.asset(for: object) else { continue }
                let node = SCNNode(); node.name = object.id.uuidString
                node.addChildNode(asset.node.clone())
                if object.assetID == nil {
                    // SCNNode.clone shares geometry / materials. Each cube needs its
                    // own material so changing a color never recolors other objects.
                    node.enumerateChildNodes { child, _ in
                        guard let geometry = child.geometry?.copy() as? SCNGeometry else { return }
                        geometry.materials = geometry.materials.compactMap { $0.copy() as? SCNMaterial }
                        child.geometry = geometry
                    }
                }
                content.addChildNode(node); nodes[object.id] = node; nodeAssets[object.id] = object.assetID
            }
            if object.assetID == nil {
                let color = object.resolvedCubeColor.nsColor
                nodes[object.id]?.enumerateChildNodes { child, _ in
                    child.geometry?.materials.forEach { $0.diffuse.contents = color }
                }
            }
            let m = object.transform.matrix
            nodes[object.id]?.simdTransform = simd_float4x4(
                SIMD4<Float>(m.columns.0), SIMD4<Float>(m.columns.1), SIMD4<Float>(m.columns.2), SIMD4<Float>(m.columns.3))
            nodes[object.id]?.isHidden = !object.isVisible
        }
    }
}

private struct EditorSceneSurface: NSViewRepresentable {
    @ObservedObject var lab: LabModel
    @ObservedObject var editor: EditorModel
    func makeCoordinator() -> EditorSceneRenderer { EditorSceneRenderer() }
    func makeNSView(context: Context) -> EditorSceneContainer {
        let container = EditorSceneContainer()
        let view = container.sceneView
        view.scene = context.coordinator.scene; view.pointOfView = context.coordinator.cameraNode
        view.allowsCameraControl = false; view.autoenablesDefaultLighting = false
        view.backgroundColor = .clear; view.antialiasingMode = .multisampling4X
        return container
    }
    func updateNSView(_ view: EditorSceneContainer, context: Context) {
        do {
            try context.coordinator.update(camera: lab.camera, meshes: { try lab.scene?.meshes() ?? [] },
                                           revision: lab.revision, editor: editor)
        } catch {
            let message = error.localizedDescription
            Task { @MainActor in editor.error = message }
        }
    }
}

final class EditorSceneContainer: NSView {
    let sceneView = SCNView(frame: NSRect(x: 0, y: 0, width: 1, height: 1))
    init() {
        super.init(frame: NSRect(x: 0, y: 0, width: 1, height: 1))
        addSubview(sceneView)
    }
    required init?(coder: NSCoder) { nil }
    override func layout() {
        super.layout()
        let size = bounds.size
        guard size.width.isFinite, size.height.isFinite, size.width > 0, size.height > 0 else {
            sceneView.isHidden = true
            return
        }
        sceneView.frame = bounds
        sceneView.isHidden = false
    }
}

private struct EditorRasterSurface: NSViewRepresentable {
    let image: CGImage
    let nearest: Bool
    func makeNSView(context: Context) -> EditorRasterImageView {
        let view = EditorRasterImageView()
        view.imageScaling = .scaleAxesIndependently
        return view
    }
    func updateNSView(_ view: EditorRasterImageView, context: Context) {
        view.nearest = nearest
        view.image = NSImage(cgImage: image, size: NSSize(width: image.width, height: image.height))
    }
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: EditorRasterImageView, context: Context) -> CGSize? {
        // The enclosing frame already fits the camera raster while preserving its aspect.
        // NSImageView's default sizing would keep the decoded image's intrinsic pixel size
        // as a minimum, cropping large captures and misaligning them with the scene / gizmo.
        guard let width = proposal.width, let height = proposal.height,
              width.isFinite, height.isFinite else { return nil }
        return CGSize(width: max(0, width), height: max(0, height))
    }
}
private final class EditorRasterImageView: NSImageView {
    var nearest = false
    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.current?.imageInterpolation = nearest ? .none : .high
        super.draw(dirtyRect)
    }
}

private struct EditorInputSurface: NSViewRepresentable {
    let down: (CGPoint) -> Void
    let drag: (CGPoint, Bool) -> Void
    let up: () -> Void
    let hover: (CGPoint?) -> Void
    let key: (NSEvent) -> Bool
    func makeNSView(context: Context) -> EditorInputView { EditorInputView() }
    func updateNSView(_ view: EditorInputView, context: Context) { view.callbacks = self }
}
private final class EditorInputView: NSView {
    var callbacks: EditorInputSurface?
    override var acceptsFirstResponder: Bool { true }
    override var isFlipped: Bool { true }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self); callbacks?.down(convert(event.locationInWindow, from: nil))
    }
    override func mouseDragged(with event: NSEvent) { callbacks?.drag(convert(event.locationInWindow, from: nil), event.modifierFlags.contains(.shift)) }
    override func mouseUp(with event: NSEvent) { callbacks?.up() }
    override func mouseMoved(with event: NSEvent) { callbacks?.hover(convert(event.locationInWindow, from: nil)) }
    override func mouseExited(with event: NSEvent) { callbacks?.hover(nil) }
    override func keyDown(with event: NSEvent) { if callbacks?.key(event) != true { super.keyDown(with: event) } }
}

struct EditorViewport: View {
    @ObservedObject var lab: LabModel
    @ObservedObject var editor: EditorModel
    @State private var drag: EditorGizmoDrag?
    @State private var hovered: EditorGizmoHandle?
    var body: some View {
        GeometryReader { geometry in
            let camera = lab.camera
            let layout = InspectorRasterLayout(rasterSize: CGSize(width: Int(camera?.width ?? 640), height: Int(camera?.height ?? 480)),
                                               viewportSize: geometry.size, quarterTurns: lab.rasterQuarterTurns)
            let projection = editor.selected.flatMap { object in
                object.isVisible ? camera.flatMap { EditorGizmoProjection(object: object, tool: editor.tool, space: editor.space, camera: $0, layout: layout) } : nil
            }
            let background = editor.shading == .image ? lab.image : editor.shading == .depth ? lab.depthImage : nil
            ZStack {
                Color(red: 0.045, green: 0.05, blue: 0.06)
                if let background {
                    EditorRasterSurface(image: background, nearest: editor.shading == .depth)
                        .frame(width: layout.sourceSize.width, height: layout.sourceSize.height)
                        .rotationEffect(.degrees(Double(layout.quarterTurns) * 90))
                        .allowsHitTesting(false)
                }
                EditorSceneSurface(lab: lab, editor: editor)
                    .frame(width: layout.sourceSize.width, height: layout.sourceSize.height)
                    .rotationEffect(.degrees(Double(layout.quarterTurns) * 90))
                    .allowsHitTesting(false)
                EditorGizmoOverlay(projection: projection, active: drag?.handle ?? hovered,
                    bounds: projectedBounds(camera: camera, layout: layout),
                    placement: editor.placement.flatMap { point($0.point, camera: camera, layout: layout) })
                    .frame(width: geometry.size.width, height: geometry.size.height)
                EditorInputSurface(down: { pointerDown($0, projection: projection, layout: layout) },
                    drag: { pointerDragged($0, shift: $1, layout: layout) },
                    up: { editor.finishDrag(); drag = nil },
                    hover: { hovered = $0.flatMap { projection?.hit(at: $0) } }, key: handleKey)
                    .frame(width: geometry.size.width, height: geometry.size.height)
                VStack {
                    HStack {
                        Label("固定カメラ", systemImage: "lock.fill")
                        if editor.shading == .depth && lab.depthImage == nil { Text("このフレームに深度データはありません") }
                        if editor.shading == .image && lab.image == nil { Text(lab.decodeError ?? "映像なし") }
                        Spacer()
                        Text("X").foregroundStyle(.red); Text("Y").foregroundStyle(.green); Text("Z").foregroundStyle(.blue)
                    }.font(.caption).padding(12).background(.black.opacity(0.35))
                    Spacer()
                }.allowsHitTesting(false)
            }.frame(width: geometry.size.width, height: geometry.size.height).clipped()
        }
        .onChange(of: lab.revision) { _, _ in
            if drag != nil { editor.finishDrag(cancel: true); drag = nil }
        }
    }
    private func point(_ point: SIMD3<Double>, camera: Camera?, layout: InspectorRasterLayout) -> CGPoint? {
        guard let pixel = try? camera?.project(point) else { return nil }
        return layout.viewPoint(for: CGPoint(x: pixel.x, y: pixel.y))
    }
    private func projectedBounds(camera: Camera?, layout: InspectorRasterLayout) -> [CGPoint?] {
        guard let object = editor.selected, object.isVisible, let asset = editor.asset(for: object) else { return [] }
        return EditorMath.cubeVertices.map {
            point(object.transform.position + object.transform.rotation.act($0 * asset.dimensions * object.transform.scale), camera: camera, layout: layout)
        }
    }
    private func pointerDown(_ point: CGPoint, projection: EditorGizmoProjection?, layout: InspectorRasterLayout) {
        guard !editor.isLoading, !lab.isBusy, let camera = lab.camera, let scene = lab.scene,
              let pixel = layout.pixel(at: point), let ray = try? EditorMath.ray(camera: camera, pixel: SIMD2(pixel.x, pixel.y)) else { return }
        if let projection, let object = editor.selected, let handle = projection.hit(at: point) {
            editor.beginDrag()
            drag = EditorGizmoDrag(handle: handle, object: object, projection: projection, point: point, ray: ray)
            return
        }
        let maximum = (try? scene.raycast(origin: ray.origin, direction: ray.direction)?.distanceMeters) ?? 10_000
        if let object = editor.pickObject(ray: ray, maximumDistance: maximum + 0.001) {
            editor.selectedID = object
        } else {
            editor.selectedID = nil
            editor.setPlacement(pixel: SIMD2(pixel.x, pixel.y), scene: scene, camera: camera)
        }
    }
    private func pointerDragged(_ point: CGPoint, shift: Bool, layout: InspectorRasterLayout) {
        guard var current = drag, let camera = lab.camera, let pixel = layout.pixel(at: point),
              let ray = try? EditorMath.ray(camera: camera, pixel: SIMD2(pixel.x, pixel.y)) else { return }
        if current.handle == .center && current.tool == .translate {
            if let hit = try? lab.scene?.raycast(pixel: SIMD2(pixel.x, pixel.y)) {
                let attachment = EditorMath.facingAttachment(hit: hit, camera: camera)
                editor.updateSelected({ $0.attachment = attachment }, maintainContact: true, duringDrag: true)
            }
        } else if let transform = current.update(point: point, ray: ray, snap: shift || editor.snapping) {
            editor.updateSelected({
                $0.transform = transform
                if current.tool == .translate { $0.attachment = nil }
            }, maintainContact: current.tool != .translate, duringDrag: true)
        }
        drag = current
    }
    private func handleKey(_ event: NSEvent) -> Bool {
        if event.keyCode == 53 { editor.finishDrag(cancel: true); drag = nil; return true }
        guard drag == nil, !editor.isLoading else { return false }
        let key = event.charactersIgnoringModifiers?.lowercased()
        if event.modifierFlags.contains(.command) {
            if key == "z" { event.modifierFlags.contains(.shift) ? editor.redo() : editor.undo(); return true }
            if key == "d" { editor.duplicateSelected(); return true }
            if key == "s" { editor.save(); return true }
            return false
        }
        if [51, 117].contains(event.keyCode) { editor.deleteSelected(); return true }
        switch key {
        case "g": editor.tool = .translate
        case "r": editor.tool = .rotate
        case "s": editor.tool = .scale
        case "d" where event.modifierFlags.contains(.shift): editor.duplicateSelected()
        default: return false
        }
        return true
    }
}
#endif
