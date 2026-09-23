import Foundation

public enum AppleMediaError: Error, LocalizedError {
    case unsupported(String)
    case invalid(String)
    case codec(String)
    case backpressure
    public var errorDescription: String? {
        switch self {
        case .unsupported(let message), .invalid(let message), .codec(let message): return message
        case .backpressure: return "映像エンコーダーがフレームを受け付けませんでした. "
        }
    }
}
