#if os(macOS)
import SwiftUI
import SceneKit
import SpatialSnapshot
import SpatialSnapshotAppleMedia

@MainActor
final class EditorModel: ObservableObject {
    @Published private(set) var project: EditorProject?
    @Published var selectedID: UUID?
    @Published var shading: EditorShading = .image
    @Published var tool: EditorTool = .translate
    @Published var space: EditorSpace = .scene
    @Published var snapping = false
    @Published private(set) var isLoading = false
    @Published private(set) var isDirty = false
    @Published private(set) var revision = UUID()
    @Published var error: String?
    @Published var status = "床・壁をクリックして追加位置を選択"
    @Published private(set) var placement: EditorAttachment?
    @Published private(set) var undoCount = 0
    @Published private(set) var redoCount = 0
    private var store: EditorProjectStore?
    private let cube = EditorModelAsset.cube()
    private var assets: [UUID: EditorModelAsset] = [:]
    private var undoStack: [[EditorObject]] = []
    private var redoStack: [[EditorObject]] = []
    private var dragStart: [EditorObject]?
    private var generation = UUID()
    private final class ImportDraft {
        let reference: EditorAssetReference
        let attachment: EditorAttachment
        init(reference: EditorAssetReference, attachment: EditorAttachment) {
            self.reference = reference; self.attachment = attachment
        }
    }

    var objects: [EditorObject] { project?.objects ?? [] }
    var selected: EditorObject? { objects.first { $0.id == selectedID } }
    func asset(for object: EditorObject) -> EditorModelAsset? { object.assetID.flatMap { assets[$0] } ?? (object.assetID == nil ? cube : nil) }

    func bind(to capture: AppleSpatialAsset?, directory: URL) async {
        let id = capture?.document.streamID.map { String(format: "%02x", $0) }.joined()
        guard id != project?.captureID || (capture == nil && project != nil) else { return }
        generation = UUID(); let request = generation
        project = nil; store = nil; assets = [:]; selectedID = nil; placement = nil
        undoStack = []; redoStack = []; dragStart = nil; updateHistory()
        isDirty = false; revision = UUID(); error = nil
        guard let id else { isLoading = false; return }
        isLoading = true
        do {
            let nextStore = EditorProjectStore(directory: directory.appendingPathComponent(id, isDirectory: true))
            let nextProject = try nextStore.load(captureID: id)
            var loaded: [UUID: EditorModelAsset] = [:]
            for reference in nextProject.assets {
                loaded[reference.id] = try await EditorModelAsset.load(nextStore.assetURL(reference))
                guard generation == request else { return }
            }
            guard generation == request else { return }
            store = nextStore; project = nextProject; assets = loaded
            selectedID = nextProject.objects.first?.id
            status = "床・壁をクリックして追加位置を選択"
            revision = UUID()
        } catch {
            guard generation == request else { return }
            self.error = error.localizedDescription
        }
        if generation == request { isLoading = false }
    }
    func setPlacement(pixel: SIMD2<Double>, scene: SpatialScene, camera: Camera) {
        do {
            guard let hit = try scene.raycast(pixel: pixel) else {
                status = "この位置に配置できるメッシュがありません"; return
            }
            placement = EditorMath.facingAttachment(hit: hit, camera: camera)
            status = "追加位置: \(hit.classification) · キューブまたは glTF を追加"
        } catch { self.error = error.localizedDescription }
    }
    private func insertionAttachment(scene: SpatialScene?, camera: Camera?) throws -> EditorAttachment {
        if let placement { return placement }
        if let scene, let camera, let hit = try scene.raycast(pixel: SIMD2(Double(camera.cx), Double(camera.cy))) {
            let attachment = EditorMath.facingAttachment(hit: hit, camera: camera)
            placement = attachment
            return attachment
        }
        throw EditorError.invalid("配置先の床・壁のメッシュをクリックしてから追加してください. ")
    }
    func addCube(scene: SpatialScene?, camera: Camera?) {
        guard project != nil, !isLoading else { return }
        do {
            let attachment = try insertionAttachment(scene: scene, camera: camera)
            var object = EditorObject(name: "Cube \(objects.filter { $0.assetID == nil }.count + 1)")
            object.transform.scale = SIMD3(repeating: 0.25)
            object.attachment = attachment
            object.transform.position = EditorMath.contactPosition(vertices: cube.vertices, transform: object.transform, attachment: attachment)
            recordUndo(); project?.objects.append(object); selectedID = object.id
            changed(); status = "キューブを追加しました"
        } catch { self.error = error.localizedDescription }
    }
    func importGLTF(_ url: URL, scene: SpatialScene?, camera: Camera?) async {
        guard let store, project != nil, !isLoading else { return }
        let request = generation
        do {
            let draft = try prepareImport(url, store: store, scene: scene, camera: camera)
            isLoading = true
            defer { if generation == request { isLoading = false } }
            let reference = draft.reference
            let model: EditorModelAsset
            do { model = try await EditorModelAsset.load(store.assetURL(reference)) }
            catch {
                if let assetURL = try? store.assetURL(reference) { try? FileManager.default.removeItem(at: assetURL.deletingLastPathComponent()) }
                throw error
            }
            guard generation == request else {
                if let assetURL = try? store.assetURL(reference) { try? FileManager.default.removeItem(at: assetURL.deletingLastPathComponent()) }
                return
            }
            completeImport(draft, model: model)
        } catch { if generation == request { self.error = error.localizedDescription } }
    }
    private func prepareImport(_ url: URL, store: EditorProjectStore, scene: SpatialScene?, camera: Camera?) throws -> ImportDraft {
        let attachment = try insertionAttachment(scene: scene, camera: camera)
        return ImportDraft(reference: try store.importAsset(from: url), attachment: attachment)
    }
    private func completeImport(_ draft: ImportDraft, model: EditorModelAsset) {
        let reference = draft.reference
        var object = EditorObject(name: reference.name, assetID: reference.id)
        object.attachment = draft.attachment
        object.transform.position = EditorMath.contactPosition(vertices: model.vertices, transform: object.transform, attachment: draft.attachment)
        assets[reference.id] = model
        recordUndo(); project?.assets.append(reference); project?.objects.append(object)
        selectedID = object.id; changed(); status = "\(reference.name) を追加しました"
    }
    func attachSelected(scene: SpatialScene?, camera: Camera?) {
        do {
            let attachment = try insertionAttachment(scene: scene, camera: camera)
            updateSelected({ $0.attachment = attachment }, maintainContact: true)
        } catch { self.error = error.localizedDescription }
    }
    func updateSelected(_ edit: (inout EditorObject) -> Void, maintainContact: Bool = false, duringDrag: Bool = false) {
        guard let index = project?.objects.firstIndex(where: { $0.id == selectedID }), let old = selected else { return }
        var object = old; edit(&object)
        guard object.transform.isValid else { error = "座標・角度には有限値, スケールには 0 以外の値を指定してください. "; return }
        guard object.cubeColor?.isValid ?? true else { error = "色の RGB 成分は 0〜1 の有限値を指定してください. "; return }
        if maintainContact, let attachment = object.attachment, let model = asset(for: object) {
            object.transform.position = EditorMath.contactPosition(vertices: model.vertices, transform: object.transform, attachment: attachment)
        }
        guard object != old else { return }
        if !duringDrag { recordUndo() }
        project?.objects[index] = object
        changed(save: !duringDrag)
    }
    func duplicateSelected() {
        guard var object = selected else { return }
        recordUndo(); object.id = UUID(); object.name += " copy"
        var offset = SIMD3<Double>(0.1, 0, 0)
        if var attachment = object.attachment {
            offset -= attachment.normal * simd_dot(offset, attachment.normal)
            if simd_length(offset) < 0.01 { offset = SIMD3(0, 0.1, 0) - attachment.normal * attachment.normal.y * 0.1 }
            attachment.point += offset; object.attachment = attachment
        }
        object.transform.position += offset
        project?.objects.append(object); selectedID = object.id; changed()
    }
    func deleteSelected() {
        guard let id = selectedID, objects.contains(where: { $0.id == id }) else { return }
        recordUndo(); project?.objects.removeAll { $0.id == id }; selectedID = nil; changed()
    }
    func beginDrag() { dragStart = objects }
    func finishDrag(cancel: Bool = false) {
        guard let start = dragStart else { return }
        dragStart = nil
        if cancel { project?.objects = start; changed(); return }
        guard objects != start else { return }
        undoStack.append(start); redoStack = []; updateHistory(); save()
    }
    func undo() {
        guard !isLoading, let previous = undoStack.popLast() else { return }
        redoStack.append(objects); project?.objects = previous
        if !objects.contains(where: { $0.id == selectedID }) { selectedID = objects.last?.id }
        updateHistory(); changed()
    }
    func redo() {
        guard !isLoading, let next = redoStack.popLast() else { return }
        undoStack.append(objects); project?.objects = next; selectedID = next.last?.id
        updateHistory(); changed()
    }
    func save() {
        guard let project, let store else { return }
        do { try store.save(project); isDirty = false }
        catch { isDirty = true; self.error = "編集内容を保存できません. \n" + error.localizedDescription }
    }
    func pickObject(ray: EditorRay, maximumDistance: Double) -> UUID? {
        var best = maximumDistance, selected: UUID?
        for object in objects where object.isVisible {
            guard let model = asset(for: object) else { continue }
            let inverse = simd_inverse(object.transform.matrix)
            let origin = inverse * SIMD4(ray.origin.x, ray.origin.y, ray.origin.z, 1)
            let direction = inverse * SIMD4(ray.direction.x, ray.direction.y, ray.direction.z, 0)
            let local = EditorRay(origin: SIMD3(origin.x, origin.y, origin.z), direction: SIMD3(direction.x, direction.y, direction.z))
            if let distance = model.intersection(ray: local, maximumDistance: best) {
                best = distance; selected = object.id
            }
        }
        return selected
    }
    private func recordUndo() { undoStack.append(objects); redoStack = []; updateHistory() }
    private func updateHistory() { undoCount = undoStack.count; redoCount = redoStack.count }
    private func changed(save shouldSave: Bool = true) { isDirty = true; revision = UUID(); if shouldSave { save() } }
}
#endif
