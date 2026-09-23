import SwiftUI

@main
struct SpatialSnapshotLabApp: App {
    @StateObject private var model = LabModel()
    var body: some Scene {
        WindowGroup {
            LabRootView(model: model)
                .tint(.mint)
                .preferredColorScheme(.dark)
                .onOpenURL { url in Task { await model.importFile(url) } }
                .task {
                    if model.asset == nil { await model.openDemo(movie: false) }
                }
        }
        #if os(macOS)
        .defaultSize(width: 1240, height: 840)
        #endif
    }
}
