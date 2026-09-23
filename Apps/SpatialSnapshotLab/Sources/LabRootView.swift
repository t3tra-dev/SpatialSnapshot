#if canImport(SwiftUI)
import SwiftUI
import UniformTypeIdentifiers

struct LabRootView: View {
    @ObservedObject var model: LabModel
    @State private var importing = false
    @State private var sectionSelection: LabSection?
    #if os(macOS)
    @StateObject private var editor = EditorModel()
    #endif
    init(model: LabModel) {
        self.model = model
        _sectionSelection = State(initialValue: model.section)
    }
    var body: some View {
        NavigationSplitView {
            List(selection: $sectionSelection) {
                Section {
                    Label("Inspector", systemImage: "viewfinder").tag(LabSection.inspector)
                    #if os(iOS)
                    Label("Recorder", systemImage: "record.circle").tag(LabSection.recorder)
                    #else
                    Label("Recorder", systemImage: "record.circle")
                        .foregroundStyle(.secondary).disabled(true).help("収録には ARKit 対応の iPhone / iPad が必要です")
                    Label("Editor", systemImage: "cube.transparent").tag(LabSection.editor)
                    #endif
                }
                Section("デモ") {
                    Button { Task { await model.openDemo(movie: false) } } label: {
                        Label("Room · 写真", systemImage: "photo")
                    }
                    Button { Task { await model.openDemo(movie: true) } } label: {
                        Label("Room · 動画", systemImage: "film")
                    }
                }
                if !model.recentFiles.isEmpty {
                    Section("ライブラリ") {
                        ForEach(model.recentFiles, id: \.self) { url in
                            Button { Task { await model.open(url) } } label: {
                                Label(url.deletingPathExtension().lastPathComponent,
                                    systemImage: url.pathExtension == "mov" ? "film" : "cube.transparent")
                                    .lineLimit(1)
                            }
                            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                Button(role: .destructive) { model.deleteCapture(url) } label: {
                                    Label("削除", systemImage: "trash")
                                }
                                .disabled(model.isBusy)
                            }
                        }
                    }
                }
                Section {
                    Button { importing = true } label: { Label("ファイルを開く", systemImage: "folder.badge.plus") }
                }
            }
            .listStyle(.sidebar)
            .navigationTitle("SpatialSnapshot")
            .safeAreaInset(edge: .bottom) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("LAB / v1").font(.system(.caption, design: .monospaced)).foregroundStyle(.mint)
                    Text("映像の向こう側にある空間を検査する").font(.caption2).foregroundStyle(.secondary)
                }.frame(maxWidth: .infinity, alignment: .leading).padding()
            }
            .navigationSplitViewColumnWidth(min: 200, ideal: 230, max: 280)
        } detail: {
            Group {
                #if os(macOS)
                if model.section == .editor { EditorView(lab: model, editor: editor) }
                else { InspectorView(model: model) }
                #else
                InspectorView(model: model)
                #endif
            }
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button { importing = true } label: { Label("開く", systemImage: "folder") }
                        .keyboardShortcut("o")
                }
            }
        }
        .onChange(of: model.section) { _, value in if sectionSelection != value { sectionSelection = value } }
        .task(id: sectionSelection) {
            guard !Task.isCancelled, model.section != sectionSelection else { return }
            model.section = sectionSelection
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.heic, .heif, .quickTimeMovie,
            UTType(filenameExtension: "ssps") ?? .data]) { result in
                switch result {
                case .success(let url): Task { await model.importFile(url) }
                case .failure(let error): model.report(error)
                }
            }
        .alert("操作を完了できません", isPresented: Binding(get: { model.error != nil },
            set: { if !$0 { model.error = nil } })) {
                Button("閉じる", role: .cancel) { model.error = nil }
            } message: { Text(model.error ?? "") }
        .overlay {
            if model.isBusy {
                ProgressView("処理中…").padding(24).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
            }
        }
        #if os(iOS)
        .fullScreenCover(isPresented: Binding(
            get: { model.section == .recorder },
            set: { if !$0 { model.section = .inspector } }
        )) {
            RecorderView(model: model)
                .tint(.mint)
                .preferredColorScheme(.dark)
        }
        #endif
    }
}
#endif
