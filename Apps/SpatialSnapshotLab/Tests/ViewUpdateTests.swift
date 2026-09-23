#if os(macOS)
import XCTest
import SwiftUI
import Combine

final class ViewUpdateTests: XCTestCase {
    private func requireAppKit() throws {
        guard ProcessInfo.processInfo.environment["SPATIALSNAPSHOT_MEDIA_TESTS"] == "1" else {
            throw XCTSkip("Requires AppKit / Metal services; enable with the Apple media tests.")
        }
    }
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("SpatialSnapshot-view-updates-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    @MainActor
    private func model(in root: URL) async throws -> LabModel {
        let model = LabModel(libraryDirectory: root)
        let room = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("Resources/Room.heic")
        await model.open(room)
        XCTAssertNil(model.error)
        return model
    }
    @MainActor
    private func window<V: View>(_ view: V) -> NSWindow {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1240, height: 840),
                              styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.contentView = NSHostingView(rootView: view.preferredColorScheme(.dark))
        window.contentView?.layoutSubtreeIfNeeded()
        return window // Test-owned, never ordered onscreen or made key.
    }
    @MainActor
    private func settle(_ window: NSWindow) async throws {
        // Offscreen hosting views need explicit layout passes on both sides of the
        // deferred model update; an onscreen window gets these from AppKit.
        for _ in 0..<3 {
            try await Task.sleep(for: .milliseconds(30))
            window.contentView?.layoutSubtreeIfNeeded()
            window.contentView?.displayIfNeeded()
        }
    }
    @MainActor
    private func descendants<T: NSView>(_ view: NSView, of type: T.Type) -> [T] {
        ((view as? T).map { [$0] } ?? []) + view.subviews.flatMap { descendants($0, of: type) }
    }
    @MainActor
    func testEditorNativePickerPublishesAfterControlCallbackAndTracksModel() async throws {
        try requireAppKit()
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let lab = try await model(in: root), editor = EditorModel()
        await editor.bind(to: lab.asset, directory: try lab.editorProjectDirectory())
        editor.addCube(scene: lab.scene, camera: lab.camera)
        let window = window(EditorView(lab: lab, editor: editor)); defer { window.contentView = nil }
        try await settle(window)
        let picker = try XCTUnwrap(descendants(window.contentView!, of: NSSegmentedControl.self)
            .first { $0.segmentCount == 3 && $0.label(forSegment: 0) == "画像" })
        var inCallback = false, synchronousPublications = 0
        let subscription = editor.objectWillChange.sink { if inCallback { synchronousPublications += 1 } }
        defer { subscription.cancel() }
        for (index, expected) in [(1, EditorShading.depth), (2, .wireframe), (0, .image)] {
            inCallback = true
            picker.selectedSegment = index
            XCTAssertTrue(picker.sendAction(picker.action, to: picker.target))
            inCallback = false
            try await settle(window)
            XCTAssertEqual(editor.shading, expected)
        }
        XCTAssertEqual(synchronousPublications, 0)
        editor.shading = .depth
        try await settle(window)
        XCTAssertEqual(picker.selectedSegment, 1)
        editor.duplicateSelected()
        try await settle(window)
        let selected = editor.selectedID
        XCTAssertNotNil(selected)
        XCTAssertEqual(editor.objects.count, 2)
        let outliner = try XCTUnwrap(descendants(window.contentView!, of: NSTableView.self)
            .first { $0.numberOfRows == 2 })
        let firstID = editor.objects[0].id
        editor.selectedID = firstID
        try await settle(window)
        let firstRow = outliner.selectedRow
        XCTAssertGreaterThanOrEqual(firstRow, 0)
        editor.selectedID = selected
        try await settle(window)
        inCallback = true
        outliner.selectRowIndexes(IndexSet(integer: firstRow), byExtendingSelection: false)
        inCallback = false
        try await settle(window)
        XCTAssertEqual(editor.selectedID, firstID)
        XCTAssertEqual(synchronousPublications, 0)
        editor.shading = .wireframe
        try await settle(window)
        XCTAssertEqual(editor.selectedID, firstID)
        editor.deleteSelected()
        try await settle(window)
        XCTAssertNil(editor.selectedID)
    }
    @MainActor
    func testCubeColorPickerDefersPublicationAndTracksUndoAndSelection() async throws {
        try requireAppKit()
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let lab = try await model(in: root), editor = EditorModel()
        await editor.bind(to: lab.asset, directory: try lab.editorProjectDirectory())
        editor.addCube(scene: lab.scene, camera: lab.camera)
        let firstID = try XCTUnwrap(editor.selectedID)
        editor.duplicateSelected()
        let secondID = try XCTUnwrap(editor.selectedID), history = editor.undoCount
        let window = window(EditorView(lab: lab, editor: editor)); defer { window.contentView = nil }
        try await settle(window)
        XCTAssertEqual(editor.undoCount, history)
        func well() throws -> NSColorWell {
            try XCTUnwrap(descendants(window.contentView!, of: NSColorWell.self).first)
        }
        var inCallback = false, synchronousPublications = 0
        let subscription = editor.objectWillChange.sink { if inCallback { synchronousPublications += 1 } }
        defer { subscription.cancel() }
        let custom = EditorColor(red: 0.13, green: 0.57, blue: 0.91)
        let picker = try well()
        inCallback = true
        picker.color = custom.nsColor
        XCTAssertTrue(picker.sendAction(picker.action, to: picker.target))
        inCallback = false
        try await settle(window)
        XCTAssertEqual(synchronousPublications, 0)
        XCTAssertTrue(try XCTUnwrap(editor.selected?.cubeColor).matches(custom))
        XCTAssertEqual(editor.objects.first?.resolvedCubeColor, .defaultCube)
        XCTAssertEqual(editor.undoCount, history + 1)
        editor.undo()
        try await settle(window)
        XCTAssertTrue(try XCTUnwrap(EditorColor.from(well().color)).matches(.defaultCube))
        XCTAssertEqual(editor.undoCount, history)
        editor.redo()
        try await settle(window)
        XCTAssertTrue(try XCTUnwrap(EditorColor.from(well().color)).matches(custom))
        editor.selectedID = firstID
        try await settle(window)
        XCTAssertTrue(try XCTUnwrap(EditorColor.from(well().color)).matches(.defaultCube))
        // A pending color action from a removed selection must not recolor the new one.
        let oldPicker = try well()
        oldPicker.color = NSColor.red
        XCTAssertTrue(oldPicker.sendAction(oldPicker.action, to: oldPicker.target))
        editor.selectedID = secondID
        try await settle(window)
        XCTAssertTrue(try XCTUnwrap(editor.selected?.cubeColor).matches(custom))
        XCTAssertEqual(editor.objects.first?.resolvedCubeColor, .defaultCube)
        XCTAssertEqual(editor.undoCount, history + 1)
    }
    @MainActor
    func testSidebarNativeSelectionPublishesAfterCallbackAndHasOneSplitView() async throws {
        try requireAppKit()
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let model = try await model(in: root)
        let window = window(LabRootView(model: model)); defer { window.contentView = nil }
        try await settle(window)
        let tables = descendants(window.contentView!, of: NSTableView.self)
        let sidebar = try XCTUnwrap(tables.first { $0.numberOfRows >= 3 })
        // SwiftUI inserts section / spacing rows; use the actual tagged row positions.
        model.section = .inspector
        try await settle(window)
        XCTAssertEqual(model.section, .inspector)
        let inspectorRow = sidebar.selectedRow
        model.section = .editor
        try await settle(window)
        XCTAssertEqual(model.section, .editor)
        let editorRow = sidebar.selectedRow
        XCTAssertGreaterThanOrEqual(inspectorRow, 0)
        XCTAssertGreaterThanOrEqual(editorRow, 0)
        XCTAssertNotEqual(inspectorRow, editorRow)
        var inCallback = false, synchronousPublications = 0
        let subscription = model.objectWillChange.sink { if inCallback { synchronousPublications += 1 } }
        defer { subscription.cancel() }
        for (row, expected) in [(inspectorRow, LabSection.inspector), (editorRow, .editor)] {
            inCallback = true
            sidebar.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            inCallback = false
            try await settle(window)
            XCTAssertEqual(model.section, expected)
        }
        XCTAssertEqual(synchronousPublications, 0)
        XCTAssertEqual(descendants(window.contentView!, of: NSSplitView.self).count, 1)
        model.section = .inspector
        try await settle(window)
        XCTAssertEqual(sidebar.selectedRow, inspectorRow)
    }
    @MainActor
    func testInspectorNativePickerPublishesAfterControlCallbackAndTracksModel() async throws {
        try requireAppKit()
        let root = try directory(); defer { try? FileManager.default.removeItem(at: root) }
        let model = try await model(in: root)
        let window = window(InspectorView(model: model)); defer { window.contentView = nil }
        try await settle(window)
        let picker = try XCTUnwrap(descendants(window.contentView!, of: NSSegmentedControl.self)
            .first { $0.segmentCount == 3 && $0.label(forSegment: 0) == "映像" })
        var inCallback = false, synchronousPublications = 0
        let subscription = model.objectWillChange.sink { if inCallback { synchronousPublications += 1 } }
        defer { subscription.cancel() }
        for (index, expected) in [(1, InspectorLayer.depth), (0, .image)] {
            inCallback = true
            picker.selectedSegment = index
            XCTAssertTrue(picker.sendAction(picker.action, to: picker.target))
            inCallback = false
            try await settle(window)
            XCTAssertEqual(model.layer, expected)
        }
        XCTAssertEqual(synchronousPublications, 0)
        model.layer = .depth
        try await settle(window)
        XCTAssertEqual(picker.selectedSegment, 1)
    }
    @MainActor
    func testSceneContainerKeepsMetalViewNonemptyDuringLayoutTransitions() throws {
        try requireAppKit()
        let container = EditorSceneContainer()
        for size in [CGSize(width: 640, height: 480), .zero, CGSize(width: 1, height: 0), CGSize(width: 480, height: 640)] {
            container.setFrameSize(size); container.layout()
            XCTAssertGreaterThan(container.sceneView.bounds.width, 0)
            XCTAssertGreaterThan(container.sceneView.bounds.height, 0)
            if size.width > 0 && size.height > 0 {
                XCTAssertFalse(container.sceneView.isHidden)
                XCTAssertEqual(container.sceneView.bounds.size, size)
            } else { XCTAssertTrue(container.sceneView.isHidden) }
        }
    }
}
#endif
