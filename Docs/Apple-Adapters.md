# Apple adapters

Swift Package の `SpatialSnapshotAppleMedia`, `SpatialSnapshotCapture`, `SpatialSnapshotRealityKit` を使用します. macOS 13 / iOS 16 以降が対象で, ARKit の収録は iOS 実機のみです. 各 adapter は `@MainActor` で利用してください.

## AppleMedia

```swift
import SpatialSnapshotAppleMedia

let asset = try AppleSpatialAsset(url: fileURL)
let image = try await asset.image(at: 0)
let scene = try asset.document.scene(at: asset.cameras[0].timestampNanoseconds)
```

`AppleSpatialAsset` は HEIF / MOV / SSPS を検証して開きます. 画像の復号失敗と空間データの読み取りは独立しています. フレームは binding が参照する映像から取得し, PTS と raster を照合します.

| API                                                     | 操作                                                     |
| ------------------------------------------------------- | -------------------------------------------------------- |
| `SpatialHEIFEncoder.encode(image:spatialData:quality:)` | 同じ raster の画像と STILL stream から HEIC を生成       |
| `SpatialMovieWriter`                                    | RGB buffer を H.264 として符号化し, VIDEO stream と結合  |
| `SpatialMovieTrimmer.trim`                              | CAMERA の時刻を境界として映像・SSPS を切り出し, 再符号化 |
| `AppleSpatialAsset.exportSpatialData`                   | SSPS 単体を保存                                          |
| `AppleSpatialAsset.exportWithoutSpatialMetadata`        | 空間メタデータを除去したメディアを保存                   |

movie writer は同じ exposure / timestamp の RGB と CAMERA を使用してください. `append` が受理した RGB に対応するサンプルだけを SSPS writer に追加し, SSPS の追加に失敗した場合は `cancel()` してください. 完成出力は検証後に atomic write します.

full-file 操作は 256 MiB, encoded video は 96 MiB, movie writer は 1800 フレーム・4096 × 4096・偶数寸法・1〜60 fps が上限です. sample interval は `UInt32` nanoseconds に収まる必要があります. trim は video と SSPS metadata の 2 track に対応します. C のリソース上限も適用されます.

## Capture

```swift
import SpatialSnapshotCapture

let pipeline = try SpatialSnapshotCapturePipeline()
try pipeline.startRecording()
// 既存 ARSession の delegate から main actor で frame を渡す.
try pipeline.ingest(frame)
try await pipeline.finishRecording(to: movieURL)
```

静止画は録画停止中に `captureStill(_:to:)` で保存します. `SpatialSnapshotRecorder` は専用の ARSession を所有する wrapper です. ホストアプリが `NSCameraUsageDescription` を設定して権限を取得し, `startSession()` を呼びます.

最初に受理したカメラが SSPS の原点となり, 画素はセンサーの raster 方向を保持します. 物理的な下方向は scene gravity で表します. 表示時の回転は画像・投影・入力座標を一緒に変換してください. container の orientation を追加すると binding の raster 条件に違反します.

LiDAR 対応機では mesh / classification / `sceneDepth` を収録します. 非対応機は RGB とカメラ姿勢を収録します. 深度は camera-space +Z の millimeters, 無効値は 0, confidence は SSPS の 1 / 2 / 3 に対応します.

既定値は最大 30 秒・15 fps, mesh 更新間隔 0.5 秒です. `Configuration.maximumMeshFaces` は nil で入力面数の上限なし, 正の値で上限を設定します. メモリ・分割処理量・SSPS の形式上の上限は別途適用されます. tracking が normal でない frame は除外します.

`meshSnapshot` / `onMeshSnapshot` は保存用に量子化された `chunks` と `worldFromScene` を公開します. 録画中は受理したフレームの保存状態, 録画前は撮影用プレビューです. 停止・失敗時は nil になります.

専用 recorder はセッション中断・失敗時に収録を破棄します. 既存 ARSession を使うホストは world origin の reset / relocalization 時に `cancel()` してください. 大きな mesh の処理では UI 更新が遅れる場合があります.

## RealityKit

```swift
import SpatialSnapshotRealityKit

let mesh = try SpatialSnapshotRealityKit.meshEntity(from: scene)
let occlusion = try SpatialSnapshotRealityKit.occlusionEntity(from: scene)
let path = SpatialSnapshotRealityKit.cameraPath(cameras)
```

`placementEntity(at:)` は SurfaceHit の配置マーカーを生成します. メッシュ・軌跡・配置点には SSPS から RealityKit への軸変換を適用します. 生成した entity はホスト所有の scene に追加してください.

`debugMeshEntity(from:)` は量子化済みの三角形を表示します. iOS 18 / macOS 15 以降はワイヤーフレーム, それ以前は半透明の面です. AR world へ重ねる場合は root transform を snapshot の `worldFromScene` で置き換えます.

アプリの操作と要件は [SpatialSnapshot Lab](../Apps/SpatialSnapshotLab/README.md) を参照してください.
