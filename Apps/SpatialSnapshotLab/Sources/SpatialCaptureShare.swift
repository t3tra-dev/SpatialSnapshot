#if canImport(CoreTransferable) && canImport(AVFoundation)
import CoreTransferable
import Foundation
import UniformTypeIdentifiers
import SpatialSnapshotAppleMedia

/// Shares the validated container as one file, including its SSPS binding.
/// No decoded image / movie representation is offered that could strip metadata.
struct SpatialCaptureShare: Transferable, Sendable {
    let url: URL
    let contentType: UTType

    @MainActor
    init?(asset: AppleSpatialAsset) {
        guard let media = asset.media else { return nil }
        url = asset.url
        contentType = media.kind == .quickTime ? .quickTimeMovie :
            (url.pathExtension.lowercased() == "heic" ? .heic : .heif)
    }

    nonisolated static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .heic) { item in
            SentTransferredFile(item.url, allowAccessingOriginalFile: false)
        }
        .exportingCondition { $0.contentType == .heic }
        .suggestedFileName { $0.url.lastPathComponent }

        FileRepresentation(exportedContentType: .heif) { item in
            SentTransferredFile(item.url, allowAccessingOriginalFile: false)
        }
        .exportingCondition { $0.contentType == .heif }
        .suggestedFileName { $0.url.lastPathComponent }

        FileRepresentation(exportedContentType: .quickTimeMovie) { item in
            SentTransferredFile(item.url, allowAccessingOriginalFile: false)
        }
        .exportingCondition { $0.contentType == .quickTimeMovie }
        .suggestedFileName { $0.url.lastPathComponent }
    }
}
#endif
