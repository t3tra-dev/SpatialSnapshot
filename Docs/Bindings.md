# Binding API

[HEIF Binding v1](../Specs/HEIF-Binding-v1.md) / [QuickTime Binding v1](../Specs/QuickTime-Binding-v1.md) の読み書き API です. 公開宣言は [bindings.h](../Sources/SpatialSnapshotC/include/spatialsnapshot/bindings.h), Swift API は `SpatialMedia` です. pixel codec は [Apple adapters](Apple-Adapters.md) で扱います.

## 読み取り

```swift
let media = try SpatialMedia(contentsOf: movieURL, kind: .quickTime)
let scene = try media.document.scene(at: frameTimestamp)
let packetStream = media.sspsData
```

`ss_container_open` は `ss_io_t.read_at` / `size`, `ss_container_open_memory` はメモリ入力を使います. 入力 bytes と I/O context は呼び出し中だけ有効であれば十分です. binding と SSPS の検証に成功した場合にのみ document を返します.

| API                           | 戻り値・所有権                                                      |
| ----------------------------- | ------------------------------------------------------------------- |
| `ss_container_document`       | 独立して release できる retained document                           |
| `ss_container_ssps_bytes`     | container の寿命内で有効な SSPS bytes                               |
| `ss_container_get_sample`     | presentation 順の時刻・duration, decode index, media / bundle range |
| `ss_container_get_checkpoint` | completed checkpoint の時刻, sample index, logical SSPS offset      |
| `ss_binding_output_bytes`     | output handle の寿命内で有効な完成 bytes                            |

container と output はそれぞれ `ss_container_release` / `ss_binding_output_release` で解放します. 失敗時の出力 handle は NULL です. record の初期化と同期の契約は [C ABI](../Specs/C-ABI.md) に従います. grid / multi-extent HEIF primary の `media_size` は 0 です.

## 書き込みと編集

| C API                       | 入力と操作                                                                   |
| --------------------------- | ---------------------------------------------------------------------------- |
| `ss_heif_bind_memory`       | 同じ capture の encoded HEIF と valid STILL document を結合                  |
| `ss_quicktime_bind_memory`  | finalized MOV と valid VIDEO document を結合                                 |
| `ss_quicktime_mux`          | visual sample-entry atom, decode 順の encoded samples, VIDEO document を mux |
| `ss_quicktime_trim_memory`  | 映像と SSPS を切り出し, 時刻・座標を rebase                                  |
| `ss_container_strip_memory` | SSPS item / track と参照を除去                                               |

MOV の movie / video timescale は 1 GHz, PTS・duration・raster は SSPS と一致する必要があります. mux の各 sample は presentation timestamp を指定します. HEIF では unbound 入力の恒等 `irot=0` を除去しますが, 非恒等変換や不正な既存 binding は拒否します.

portable trim は video + SSPS の 2 track で, 保持する全フレームが sync sample の場合に対応します. Swift では `SpatialMedia.trimmingQuickTime(_:from:to:)` を使います. 再符号化が必要な動画は `SpatialMovieTrimmer` を使用してください.

Swift のメタデータ除去は `SpatialMedia.removingSpatialMetadata` です. MOV は未参照の旧 SSPS payload が残る場合があり, 機密データの完全消去には使えません. 完成 bytes の保存には atomic write を使用してください.

## 診断と制限

`ss_binding_options_t.diagnostic` は同期 callback です. domain, status, file / SSPS offset, item / track ID, packet type, message を返します. Swift では `BindingError.diagnostics` から取得します.

host malformed, binding malformed, SSPS malformed, unsupported, resource limit, I/O error を区別します. 出力を表現できない場合は `BINDING_UNREPRESENTABLE`, 再符号化が必要な portable trim は `VIDEO_DECODER_UNAVAILABLE` です. C の対応 status は `SS_BINDING_UNREPRESENTABLE (-12)` / `SS_DECODER_UNAVAILABLE (-13)` です.

既定の memory limit は context ごとに 256 MiB です. `binding_options.resources` と, trim 時の `writer_options` により設定します. 各 binding 仕様の上限も適用されます.

以下は対応範囲外です.

- HEIF の item-offset construction method 2, protected / external data の再配置, movie / entity-group relationships を伴う再配置.
- XMP の DTD・独自 entity 宣言.
- portable trim での inter-frame codec 再符号化, audio 等の追加 track, sample dependency group.
