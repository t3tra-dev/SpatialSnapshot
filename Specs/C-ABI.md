# SpatialSnapshot C ABI 1.0.0

公開宣言は [spatialsnapshot.h](../Sources/SpatialSnapshotC/include/spatialsnapshot/spatialsnapshot.h) と, 同ヘッダーから include する [bindings.h](../Sources/SpatialSnapshotC/include/spatialsnapshot/bindings.h) です. container / output handle と診断の契約は [Binding API](../Docs/Bindings.md) を参照してください.
wire layout と C struct のメモリ配置は別物です. C struct をファイルへ直接書き込まないでください.

## Version

library release は 1.0.0, `ss_version()` と `SS_ABI_VERSION` は `0x00010000` です.
値は `major << 16 | minor << 8 | patch` として表します.
SSPS wire version は仕様の二つの u16 field に従い, 1.0 を出力します. wire version を Semantic Versioning として解釈しません.

拡張可能な public record は `struct_size` と `abi_version` で始まります.
ABI major が一致し, 既知 prefix 以上の `struct_size` を持つ record を受け付け, 末尾の追加 field は読みません.
C では `SS_INIT(type)`, options と camera ではそれぞれの `*_init` を使用してください.
allocator を含む writer options は `ss_writer_options_init` で初期化してください.

`ss_vec2_t`, `ss_vec3_t`, `ss_cell_key_t` は長さが固定された数学的 tuple であり, この prefix を持ちません.
enum に依存せず固定幅整数と定数を使います. wire の f32 は IEEE 754 binary32, scene point / ray / hit の座標演算は binary64 です. signed cell key の全範囲で u16 量子化の細かさを維持するため, geometry を float32 の scene point に丸めません.

## 所有権

| API / 値                                              | 寿命                                                            |
| ----------------------------------------------------- | --------------------------------------------------------------- |
| `ss_document_open_memory` / `ss_document_open` の入力 | 呼び出し中のみ. bytes をコピーし, I/O context を保持しない      |
| `ss_document_t`                                       | create/open の成功で reference 1. retain / release を対応させる |
| `ss_scene_cursor_t`                                   | document を retain. release 時に解放する                        |
| cursor の mesh / depth 配列                           | 次の成功した seek または cursor release まで有効                |
| document の stored packet payload                     | document の最終 release まで有効                                |
| mesh set の配列                                       | mesh set release まで有効                                       |
| writer append に渡す配列                              | append 呼び出し中のみ. 成功時に独立した正準データを保持する     |
| writer bytes                                          | finish 成功後から writer release まで有効                       |
| decoder の feed 入力                                  | feed 呼び出し中のみ. 内部 buffer にコピーする                   |
| allocator context                                     | その allocator を使う最後の handle の release まで有効          |

create/open/trim の出力 handle は失敗時 `NULL` です. 解放 API は `NULL` を許容します.
document は不変ですが, reference count, 関連 cursor の cache, allocator の予算は共有するため, 同じ document と関連 handle を同時に操作する場合は外部同期が必要です.

カスタム allocator の `allocate` は `malloc` と同じ alignment を保証してください. `allocate` / `deallocate` は両方指定するか, 両方 NULL にします. ライブラリ内の動的確保はこの allocator を経由します.

## 時刻・クエリ

timestamp は unsigned u64 nanoseconds です. seek は `[0, duration]` で geometry の状態を取得できます.
camera / depth は指定時刻と厳密に一致するサンプルに限ります. それ以外の時刻で hold-last-sample や interpolation は行いません.

mesh view の配列は native-endian, depth view の confidence は **1 pixel あたり unpacked の 1 byte** です.
wire の confidence は別途 2 bits/pixel に pack / unpack されます. quantized vertex も native-endian の xyz triple です.

projection の返す pixel は raster 外にも存在できます. Z が正でない点には `OUT_OF_RANGE` を返します.
unproject の depth は ray 長ではなく camera-space +Z の meter 値です.

raycast は winding を保持した両面交差判定で, 最も近い面を返します. direction は内部で正規化するため距離は meter です. `max_distance == 0` は上限なしです. 同距離の交差は cell key, triangle index の順で決定します.
visibility は presentation raster と persistent mesh に対する派生クエリです. depth の融合や動的物体の存在を仮定しません.

## Resource limit とエラー

`ss_open_options_t` の 0 は以下の既定値を指定します.

| Limit                                         |    既定値 |
| --------------------------------------------- | --------: |
| memory bytes                                  |   256 MiB |
| geometry bytes / state                        |   128 MiB |
| packet 数                                     | 1,000,000 |
| partition の triangle × candidate cell 処理数 | 1,000,000 |
| scene cell 数                                 |   262,144 |
| known packet raw bytes                        |    16 MiB |

memory budget は入力のコピー, 索引, 配列の拡張中の一時領域, checkpoint staging, cursor cache を含みます. cursor は document の budget を共有します. 各 geometry state はその raw payload size 合計で geometry budget を評価します.

| Status                           | 意味                                                         |
| -------------------------------- | ------------------------------------------------------------ |
| `SS_MALFORMED`                   | SSPS の構造・値・順序・必須条件への違反                      |
| `SS_UNSUPPORTED`                 | 未対応の major version / stream kind                         |
| `SS_UNSUPPORTED_CRITICAL_PACKET` | 未知の critical packet                                       |
| `SS_RESOURCE_LIMIT`              | 実装・利用者の予算, 処理量, 出力表現の上限                   |
| `SS_OUT_OF_MEMORY`               | allocator が確保に失敗                                       |
| `SS_IO_ERROR`                    | read callback / random provider が失敗                       |
| `SS_INVALID_ARGUMENT`            | API 入力, trim 区間, writer の sample が不正                 |
| `SS_OUT_OF_RANGE`                | 範囲外の時刻・pixel index・点, 再基準化後に表現できない座標  |
| `SS_NO_SAMPLE`                   | 対応する camera / depth, または有効 depth pixel が存在しない |
| `SS_NOT_FOUND`                   | ray の交差が存在しない                                       |
| `SS_INVALID_STATE`               | finish 後の書き込み, finish 前の bytes 取得など              |

open / validate / decoder finish は `ss_error_t` に status, byte offset, sequence, packet type, 診断文を返します. 未知の non-critical packet は stored CRC を検証してスキップし, その raw size でメモリを確保しません.

## Writer と trim

writer はフレーム単位の geometry batch を受け取ります. batch の key を sort し, 同一 key の二重指定を拒否します. PUT の頂点・三角形を正準化し, 同じ内容への PUT は省略します. 存在しない cell の REMOVE は拒否します.
最初は全状態の checkpoint を出力し, その後は SSPS §15.7 の時間・raw byte 上限を超える batch を全状態 checkpoint に切り替えます. 各 append と seek は失敗時に直前の状態を維持します.

stream ID 用の `random_bytes` は cryptographically strong source を使ってください. C core に弱い乱数への fallback はありません. fixture の固定 ID / deterministic callback はテスト専用です.

trim は `[start, end)` のカメラサンプルを保持します. geometry を復元し, 最初の保持 camera の inverse transform で点と pose, rotation で gravity を変換します. 全三角形を新しい grid に clip / quantize して, 初期 checkpoint と後続差分を作り直します. depth の camera-local 値を維持します. 出力は新しい ID を持ちます.

partition は量子化で潰れる面を除去し, 未参照・重複頂点と重複三角形を正準化します. 同一の正準三角形に異なる classification が重なる場合は, 未定義の優先順位を導入せず `INVALID_ARGUMENT` を返します. trim 時にこの曖昧さが発生した場合も出力を公開しません.

v1.0 writer は既知 packet のみを出力するため, trim 時の未知 non-critical extension は引き継ぎません. 異なる scene を持つ stream の単純な連結 API は提供しません.
