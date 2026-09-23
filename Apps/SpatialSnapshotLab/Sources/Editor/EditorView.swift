#if os(macOS)
import SwiftUI
import UniformTypeIdentifiers

struct EditorView: View {
    @ObservedObject var lab: LabModel
    @ObservedObject var editor: EditorModel
    @State private var controls: Controls
    @State private var outlinerSelection: UUID?
    @State private var propertiesWidth: CGFloat = 280
    @State private var resizeStart: CGFloat?

    private struct Controls: Equatable {
        var shading: EditorShading
        var space: EditorSpace
        var snapping: Bool
        @MainActor init(_ editor: EditorModel) {
            shading = editor.shading; space = editor.space; snapping = editor.snapping
        }
    }
    init(lab: LabModel, editor: EditorModel) {
        self.lab = lab; self.editor = editor
        _controls = State(initialValue: Controls(editor))
        _outlinerSelection = State(initialValue: editor.selectedID)
    }
    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if lab.asset != nil {
                editorPanes
                Divider()
                HStack {
                    Text(editor.status).lineLimit(1)
                    Spacer(minLength: 12)
                    Text("G 移動 · R 回転 · S スケール · Shift スナップ · Esc 取消")
                        .foregroundStyle(.secondary)
                }.font(.caption2).padding(.horizontal, 12).padding(.vertical, 8)
            } else {
                ContentUnavailableView("編集する収録を開く", systemImage: "cube.transparent",
                    description: Text("ライブラリまたはデモから, 空間付きの写真・動画を選択してください. "))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .background(Color(red: 0.075, green: 0.08, blue: 0.09))
        .navigationTitle("Editor")
        .onAppear { lab.pause() }
        // Native Pickers / Lists can write their selection while AppKit is updating them.
        // Keep that write in view state; publish the user's intent after the update finishes.
        .onChange(of: Controls(editor)) { _, value in if controls != value { controls = value } }
        .task(id: controls) {
            guard !Task.isCancelled else { return }
            if editor.shading != controls.shading { editor.shading = controls.shading }
            if editor.space != controls.space { editor.space = controls.space }
            if editor.snapping != controls.snapping { editor.snapping = controls.snapping }
        }
        .onChange(of: editor.selectedID) { _, value in if outlinerSelection != value { outlinerSelection = value } }
        .task(id: outlinerSelection) {
            guard !Task.isCancelled, editor.selectedID != outlinerSelection else { return }
            if outlinerSelection == nil || editor.objects.contains(where: { $0.id == outlinerSelection }) {
                editor.selectedID = outlinerSelection
            }
        }
        .task(id: lab.asset?.document.streamID) {
            do { await editor.bind(to: lab.asset, directory: try lab.editorProjectDirectory()) }
            catch { editor.error = error.localizedDescription }
        }
        .overlay {
            if editor.isLoading { ProgressView("モデルを読み込み中…").padding(20).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12)) }
        }
        .alert("編集を完了できません", isPresented: Binding(get: { editor.error != nil }, set: { if !$0 { editor.error = nil } })) {
            Button("閉じる", role: .cancel) { editor.error = nil }
        } message: { Text(editor.error ?? "") }
    }
    private var editorPanes: some View {
        GeometryReader { geometry in
            let maximum = min(360, max(250, geometry.size.width - 526))
            let width = min(maximum, max(250, propertiesWidth))
            // A nested NSSplitView inherits the NavigationSplitView sidebar's safe-area
            // inset on macOS 26 and creates conflicting leading constraints. Keep resizing
            // in SwiftUI so this detail pane owns only its own bounds.
            HStack(spacing: 0) {
                VStack(spacing: 0) {
                    viewportToolbar
                    EditorViewport(lab: lab, editor: editor).frame(minWidth: 520, minHeight: 320)
                    if lab.frameCount > 1 { frameSelector }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
                Rectangle().fill(Color.white.opacity(0.08)).frame(width: 6)
                    .contentShape(Rectangle())
                    .onHover { inside in (inside ? NSCursor.resizeLeftRight : NSCursor.arrow).set() }
                    .gesture(DragGesture(minimumDistance: 0).onChanged { value in
                        if resizeStart == nil { resizeStart = width }
                        propertiesWidth = min(maximum, max(250, (resizeStart ?? width) - value.translation.width))
                    }.onEnded { _ in resizeStart = nil })
                    .accessibilityLabel("プロパティパネルの幅")
                    .accessibilityAdjustableAction { direction in
                        propertiesWidth = min(maximum, max(250, width + (direction == .increment ? 10 : -10)))
                    }
                VStack(spacing: 0) {
                    outliner.frame(minHeight: 140, idealHeight: 200, maxHeight: 260)
                    Divider()
                    properties
                }.frame(width: width).frame(maxHeight: .infinity)
                    .background(Color.white.opacity(0.025))
            }
        }.frame(minWidth: 776, minHeight: 356)
    }
    private var header: some View {
        HStack(spacing: 14) {
            Image(systemName: "cube.transparent.fill").foregroundStyle(.mint)
            VStack(alignment: .leading, spacing: 2) {
                Text("SPATIAL / EDITOR").font(.system(.caption, design: .monospaced)).tracking(2)
                Text(lab.asset?.url.lastPathComponent ?? "収録を選択").font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            Menu {
                Button("キューブ", systemImage: "cube") { editor.addCube(scene: lab.scene, camera: lab.camera) }
                Button("glTF / GLB を読み込む…", systemImage: "shippingbox") { importModel() }
            } label: { Label("追加", systemImage: "plus") }
            .disabled(editor.project == nil || editor.isLoading)
            Button { editor.undo() } label: { Image(systemName: "arrow.uturn.backward") }
                .disabled(editor.undoCount == 0 || editor.isLoading).help("元に戻す (⌘Z)")
            Button { editor.redo() } label: { Image(systemName: "arrow.uturn.forward") }
                .disabled(editor.redoCount == 0 || editor.isLoading).help("やり直す (⇧⌘Z)")
            Button { editor.save() } label: { Label(editor.isDirty ? "保存" : "保存済み", systemImage: "square.and.arrow.down") }
                .disabled(editor.project == nil || editor.isLoading).keyboardShortcut("s")
        }.padding(12)
    }
    private var viewportToolbar: some View {
        HStack(spacing: 8) {
            ForEach(EditorTool.allCases) { tool in
                Button { editor.tool = tool } label: { Image(systemName: tool.icon).frame(width: 24, height: 24) }
                    .buttonStyle(.borderless)
                    .background(editor.tool == tool ? Color.mint.opacity(0.2) : .clear, in: RoundedRectangle(cornerRadius: 4))
                    .help("\(tool.rawValue) (\(tool.shortcut))")
            }
            Picker("座標系", selection: $controls.space) {
                ForEach(EditorSpace.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }.labelsHidden().frame(width: 85).disabled(editor.tool == .scale)
            Toggle(isOn: $controls.snapping) { Image(systemName: "grid") }
                .toggleStyle(.button).help("スナップ: 移動 5 cm / 回転 15° / スケール 0.1")
            Spacer(minLength: 4)
            Picker("ビューポート表示", selection: $controls.shading) {
                ForEach(EditorShading.allCases) { Text($0 == .wireframe ? "ワイヤー" : $0.rawValue).tag($0) }
            }.pickerStyle(.segmented).labelsHidden().frame(width: 220)
        }.controlSize(.small).frame(height: 36).padding(.horizontal, 8).disabled(editor.isLoading)
    }
    private var frameSelector: some View {
        HStack(spacing: 10) {
            Image(systemName: "film")
            Slider(value: Binding(get: { Double(lab.frameIndex) }, set: { lab.scrub(to: Int($0.rounded())) }),
                   in: 0...Double(max(1, lab.frameCount - 1)), step: 1)
            Text("\(lab.frameIndex + 1) / \(lab.frameCount)").monospacedDigit().frame(minWidth: 70)
        }.font(.caption).padding(10).disabled(editor.isLoading || lab.isBusy)
    }
    private var outliner: some View {
        VStack(spacing: 0) {
            HStack {
                Text("オブジェクト").font(.caption.weight(.semibold))
                Spacer(); Text("\(editor.objects.count)").font(.caption2).foregroundStyle(.secondary)
            }.padding(10)
            List(selection: $outlinerSelection) {
                ForEach(editor.objects) { object in
                    HStack(spacing: 8) {
                        Image(systemName: object.assetID == nil ? "cube" : "shippingbox").foregroundStyle(.orange)
                        Text(object.name).lineLimit(1)
                        Spacer()
                        Button {
                            editor.selectedID = object.id
                            editor.updateSelected { $0.isVisible.toggle() }
                        } label: { Image(systemName: object.isVisible ? "eye" : "eye.slash") }
                        .buttonStyle(.borderless).help(object.isVisible ? "非表示" : "表示")
                    }.tag(object.id)
                    .contextMenu {
                        Button("複製") { editor.selectedID = object.id; editor.duplicateSelected() }
                        Button("削除", role: .destructive) { editor.selectedID = object.id; editor.deleteSelected() }
                    }
                }
            }.listStyle(.plain).scrollContentBackground(.hidden)
            HStack {
                Button { editor.duplicateSelected() } label: { Label("複製", systemImage: "plus.square.on.square") }
                Button(role: .destructive) { editor.deleteSelected() } label: { Label("削除", systemImage: "trash") }
                Spacer()
            }.font(.caption).padding(8).disabled(editor.selected == nil)
        }.disabled(editor.isLoading)
    }
    private var properties: some View {
        ScrollView {
            if let object = editor.selected {
                VStack(alignment: .leading, spacing: 18) {
                    TextField("名前", text: Binding(get: { editor.selected?.name ?? "" }, set: { name in
                        guard editor.selectedID == object.id else { return }
                        editor.updateSelected { $0.name = name }
                    })).font(.headline).textFieldStyle(.roundedBorder)
                    if object.assetID == nil { EditorCubeColorPalette(editor: editor, object: object) }
                    vectorFields("位置", unit: "m", object: object, keyPath: \.position)
                    vectorFields("回転", unit: "°", object: object, keyPath: \.rotationDegrees)
                    vectorFields("スケール", unit: "", object: object, keyPath: \.scale)
                    Divider()
                    HStack {
                        Label(object.attachment == nil ? "自由配置" : "表面に接触", systemImage: object.attachment == nil ? "move.3d" : "link")
                        Spacer()
                        if object.attachment != nil {
                            Button("解除") { editor.updateSelected { $0.attachment = nil } }.buttonStyle(.link)
                        }
                    }.font(.caption)
                    Button("追加位置に接するよう配置") { editor.attachSelected(scene: lab.scene, camera: lab.camera) }
                        .font(.caption)
                    Text("中央ハンドルをドラッグすると床・壁に沿って移動します. 軸ハンドルと位置の数値入力では自由に移動できます. ")
                        .font(.caption2).foregroundStyle(.secondary)
                }.padding(14).id(object.id)
            } else {
                VStack(spacing: 12) {
                    Image(systemName: "cursorarrow.click.2").font(.title2)
                    Text("オブジェクトを選択").font(.subheadline)
                    Text("床・壁をクリックして追加位置を決め, 上の「追加」からキューブや glTF を配置します. ")
                        .font(.caption).multilineTextAlignment(.center)
                }.foregroundStyle(.secondary).frame(maxWidth: .infinity).padding(24)
            }
        }.disabled(editor.isLoading)
    }
    private func vectorFields(_ title: String, unit: String, object: EditorObject,
                              keyPath: WritableKeyPath<EditorTransform, SIMD3<Double>>) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack { Text(title).font(.caption.weight(.semibold)); Spacer(); Text(unit).font(.caption2).foregroundStyle(.secondary) }
            ForEach(0..<3, id: \.self) { axis in
                HStack {
                    Text(["X", "Y", "Z"][axis]).foregroundStyle([Color.red, .green, .blue][axis]).frame(width: 16)
                    EditorNumberField(value: Binding(get: {
                        editor.objects.first(where: { $0.id == object.id })?.transform[keyPath: keyPath][axis] ?? 0
                    }, set: { value in
                        guard editor.selectedID == object.id else { return }
                        editor.updateSelected({
                            $0.transform[keyPath: keyPath][axis] = value
                            if keyPath == \.position { $0.attachment = nil }
                        }, maintainContact: keyPath != \.position)
                    }))
                }.font(.system(.caption, design: .monospaced))
            }
        }
    }
    private func importModel() {
        let panel = NSOpenPanel()
        panel.title = "glTF モデルを追加"
        panel.message = "GLB, glTF, またはモデルと画像・bin が入ったフォルダを選択してください. "
        panel.allowedContentTypes = [UTType(filenameExtension: "gltf") ?? .data, UTType(filenameExtension: "glb") ?? .data]
        panel.canChooseDirectories = true; panel.canChooseFiles = true; panel.allowsMultipleSelection = false
        panel.begin { response in
            guard response == .OK, let url = panel.url else { return }
            Task { @MainActor in await editor.importGLTF(url, scene: lab.scene, camera: lab.camera) }
        }
    }
}

private struct EditorCubeColorPalette: View {
    @ObservedObject var editor: EditorModel
    let object: EditorObject
    @State private var pendingColor: EditorColor?
    private static let presets: [(name: String, color: EditorColor)] = [
        ("標準", .defaultCube),
        ("白", EditorColor(red: 0.95, green: 0.95, blue: 0.95)),
        ("グレー", EditorColor(red: 0.45, green: 0.48, blue: 0.52)),
        ("黒", EditorColor(red: 0.08, green: 0.09, blue: 0.11)),
        ("赤", EditorColor(red: 0.90, green: 0.18, blue: 0.20)),
        ("オレンジ", EditorColor(red: 1.00, green: 0.48, blue: 0.12)),
        ("黄", EditorColor(red: 0.98, green: 0.82, blue: 0.18)),
        ("緑", EditorColor(red: 0.22, green: 0.70, blue: 0.32)),
        ("ミント", EditorColor(red: 0.20, green: 0.82, blue: 0.66)),
        ("青", EditorColor(red: 0.18, green: 0.46, blue: 0.94)),
        ("紫", EditorColor(red: 0.60, green: 0.32, blue: 0.88)),
        ("ピンク", EditorColor(red: 0.94, green: 0.36, blue: 0.62))
    ]
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("カラー").font(.caption.weight(.semibold))
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 6), spacing: 6) {
                ForEach(Self.presets.indices, id: \.self) { index in
                    let preset = Self.presets[index]
                    let selected = object.resolvedCubeColor.matches(preset.color)
                    Button { pendingColor = preset.color } label: {
                        RoundedRectangle(cornerRadius: 5).fill(Color(nsColor: preset.color.nsColor))
                            .frame(height: 28)
                            .overlay {
                                RoundedRectangle(cornerRadius: 5)
                                    .strokeBorder(selected ? Color.mint : Color.white.opacity(0.25), lineWidth: selected ? 2 : 1)
                                if selected {
                                    Image(systemName: "checkmark").font(.caption.bold())
                                        .foregroundStyle(preset.color.red * 0.213 + preset.color.green * 0.715 + preset.color.blue * 0.072 > 0.6 ? .black : .white)
                                }
                            }
                    }.buttonStyle(.plain).help(preset.name)
                        .accessibilityLabel(preset.name)
                        .accessibilityAddTraits(selected ? .isSelected : [])
                }
            }
            ColorPicker("カスタム", selection: Binding(get: {
                Color(nsColor: (pendingColor ?? object.resolvedCubeColor).nsColor)
            }, set: { color in
                guard let value = EditorColor.from(NSColor(color)), !value.matches(object.resolvedCubeColor) else { return }
                pendingColor = value
            }), supportsOpacity: false).font(.caption)
        }
        // ColorPicker may write its binding during a native view update. Only
        // stage that intent here; publish / save after the update has finished.
        .task(id: pendingColor) {
            guard !Task.isCancelled, let color = pendingColor else { return }
            if editor.selectedID == object.id, let selected = editor.selected,
               selected.assetID == nil, !editor.isLoading, !selected.resolvedCubeColor.matches(color) {
                editor.updateSelected { $0.cubeColor = color }
            }
            pendingColor = nil
        }
    }
}

private struct EditorNumberField: View {
    @Binding var value: Double
    @State private var draft = ""
    @FocusState private var focused: Bool
    var body: some View {
        TextField("", text: $draft).textFieldStyle(.roundedBorder).focused($focused)
            .onAppear { draft = formatted(value) }
            .onChange(of: value) { _, value in if !focused { draft = formatted(value) } }
            .onChange(of: focused) { _, focused in if !focused { commit() } }
            .onSubmit { commit() }
    }
    private func formatted(_ number: Double) -> String { String(format: "%.4f", number) }
    private func commit() {
        if let number = Double(draft.trimmingCharacters(in: .whitespaces)), number.isFinite { value = number }
        draft = formatted(value)
    }
}
#endif
