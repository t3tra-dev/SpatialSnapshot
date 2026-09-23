# SpatialSnapshot

画像・動画に対応するカメラ姿勢, 三角形メッシュ, 深度を扱うフォーマットとライブラリです. SSPS の同じ座標系・時刻モデルを静止画と動画で共有し, HEIF / HEIC または QuickTime MOV に空間データを格納します.

## モジュール

| Product                   | 機能                                                            |
| ------------------------- | --------------------------------------------------------------- |
| SpatialSnapshotC          | C17. SSPS・コンテナの読み書き, 検証, 投影, raycast, trim        |
| SpatialSnapshot           | C ABI の Swift API                                              |
| SpatialSnapshotAppleMedia | ImageIO / AVFoundation による画像・動画の符号化, 復号, 切り出し |
| SpatialSnapshotCapture    | iOS / ARKit による収録. LiDAR のメッシュ・深度に対応            |
| SpatialSnapshotRealityKit | メッシュ, 遮蔽, カメラ軌跡, 配置マーカー                        |

C コアの実行時依存は C 標準ライブラリと数学ライブラリです. Swift Package は Swift 6.0 以降, Apple adapters は macOS 13 / iOS 16 以降を対象とします.

## 公開仕様と API

- [SSPS v1](Specs/SSPS-v1.md)：バイナリ形式, 座標系, 時刻, 検証規則
- [HEIF Binding v1](Specs/HEIF-Binding-v1.md)：静止画への格納
- [QuickTime Binding v1](Specs/QuickTime-Binding-v1.md)：動画への格納と同期
- [C ABI](Specs/C-ABI.md)：所有権, クエリ, エラー, リソース上限
- [Binding API](Docs/Bindings.md) / [Apple adapters](Docs/Apple-Adapters.md)
- [SpatialSnapshot Lab](Apps/SpatialSnapshotLab/README.md)：iOS の収録・検査, macOS の Editor・検査
- [SpatialSnapshot B3d](Apps/SpatialSnapshotB3d/README.md)：Blender でのカメラ・メッシュ・画像投影と動画再生

## ビルドとテスト

C コアには C17 compiler と CMake 3.20 以降が必要です. conformance corpus には Python 3.10 以降を使用します.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/build/install"
swift test
```

インストール対象は C ライブラリ, ヘッダー, CLI, CMake package, pkg-config 定義, LICENSE です. Swift Package の利用時は必要な product を依存先に指定してください.

Apple のメディアサービスを使うテストは, macOS で明示的に有効にします.

```sh
sh Scripts/check-apple-media.sh
sh Scripts/check-binding-interop.sh
sh Scripts/check-abi.sh
```

[GitHub Actions](.github/workflows/ci.yml) は push / pull request 時に次を実行します. 手動実行にも対応します.

| 環境                  | 検証内容                                                                                              |
| --------------------- | ----------------------------------------------------------------------------------------------------- |
| Ubuntu 24.04          | C の ASan / UBSan, conformance corpus, C ABI, 共有ライブラリのインストール, Swift API テスト          |
| macOS 15 / Xcode 26.0 | 上記に加え Capture の座標変換・Editor のロジックテスト, Lab の macOS / iOS / Simulator 署名なしビルド |

CI は runner に付属する Swift を使い, バージョンをログに記録します. macOS runner で Metal が使える場合は Apple media・RealityKit・Editor 描画の統合テストも有効にし, 使えない場合はスキップ理由をログに残します. ARKit の収録は対応実機で確認してください. Lab の署名設定と GLTFKit2 の取得方法は [起動手順](Apps/SpatialSnapshotLab/README.md#起動)を参照してください.

両環境で Blender アドオンの ctypes reader・時刻変換テストとインストール用 ZIP の作成も実行します. Blender 本体でのレンダー検証は `sh Scripts/check-b3d.sh` を使用します.

## Swift の例

```swift
import SpatialSnapshot

let writer = try SpatialWriter(kind: .still, gravity: SIMD3(0, 1, 0))
try writer.append(camera: Camera(
    width: 640, height: 480,
    fx: 500, fy: 500, cx: 319.5, cy: 239.5
))
let data = try writer.finish(durationNanoseconds: 0)
let document = try SpatialDocument(data: data)
let scene = try document.scene(at: 0)
```

VIDEO は timestamp が増加するカメラサンプルごとに, 対応する geometry と depth を `append` します. カメラ・深度は指定時刻と厳密に一致するサンプルを返し, メッシュはその時刻までの更新状態を返します.

`SpatialMedia(contentsOf:kind:)` で HEIF / MOV を開けます. C API は [spatialsnapshot.h](Sources/SpatialSnapshotC/include/spatialsnapshot/spatialsnapshot.h), Python からの利用例は [inspect.py](Examples/Python/inspect.py) を参照してください.

## 検査ツールとサンプル

```sh
build/ssvalidate Fixtures/v1/mesh-depth-still.ssps
build/ssinspect Fixtures/v1/geometry-video.ssps
build/ssdump Fixtures/v1/minimal-still.ssps
```

`Fixtures/manifest.json` と `Fixtures/bindings/manifest.json` は parser の期待結果を定義します. [binding corpus](Fixtures/bindings/README.md) には不正入力や不完全な画像 payload が含まれます. 表示用の合成サンプルは [Lab の Resources](Apps/SpatialSnapshotLab/Resources) にあります.

## 利用上の制約

同じ document と関連 cursor の呼び出しは外部で直列化してください. Swift の所有ハンドルは `Sendable` ではありません. 読み書きにはメモリ・処理量の上限があり, 詳細は C ABI と各 adapter の説明に記載しています.

C コアは画像・動画の pixel codec を含みません. LZ4 writer は literal-only block を出力するため圧縮率の改善はありません. MOV の空間メタデータ除去では, 未参照の payload bytes がファイル内に残る場合があります.

## ライセンス

Blender アドオンの `Apps/SpatialSnapshotB3d/` は [GPL-3.0-or-later](Apps/SpatialSnapshotB3d/LICENSE), それ以外の本プロジェクトのソースコード, ドキュメント, 合成 fixtures は [Apache License 2.0](LICENSE) で提供します. 同梱する第三者ソフトウェアには各ライセンスが適用されます. [第三者ライセンス](THIRD_PARTY_NOTICES.md)を参照してください.
