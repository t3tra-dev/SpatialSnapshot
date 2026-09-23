# Third-party notices

SpatialSnapshot の Apache License 2.0 は, 以下の第三者ソフトウェアのライセンスを変更しません.

| ソフトウェア                                                       | 使用箇所                                    | ライセンス                                                       |
| ------------------------------------------------------------------ | ------------------------------------------- | ---------------------------------------------------------------- |
| [GLTFKit2 0.5.15](https://github.com/warrenm/GLTFKit2/tree/0.5.15) | SwiftPM が取得し Lab に組み込む XCFramework | [MIT](Apps/SpatialSnapshotLab/Vendor/GLTFKit2/LICENSE)           |
| cgltf                                                              | GLTFKit2 に含まれる glTF parser             | [MIT](Apps/SpatialSnapshotLab/Vendor/GLTFKit2/LICENSE-cgltf.txt) |
| JSMN                                                               | cgltf に含まれる JSON parser                | [MIT](Apps/SpatialSnapshotLab/Vendor/GLTFKit2/LICENSE-jsmn.txt)  |

バイナリの配布方針・出典・チェックサムは [Vendor の説明](Apps/SpatialSnapshotLab/Vendor/GLTFKit2/README.md), アプリに含める通知全文は [ThirdPartyNotices.txt](Apps/SpatialSnapshotLab/Resources/ThirdPartyNotices.txt) にあります.

Blender アドオンやテストで外部にインストールされた Blender, LZ4, FFmpeg, libheif 等を利用する場合, それぞれのライセンスが適用されます. これらのソフトウェア自体は本リポジトリやアドオン ZIP に同梱していません.
