# SpatialSnapshot Packet Stream (SSPS) Version 1.0

**Status:** Normative specification
**Specification identifier:** `SSPS/1.0`
**File:** `Specs/SSPS-v1.md`

---

## 1. Scope

SpatialSnapshot Packet Stream (SSPS) は, 通常の静止画像または動画に対応する局所三次元空間情報を, メディアコンテナから独立した形で表現するバイナリ packet stream である.

SSPS v1.0 は次の情報を表現する.

- メディア presentation timeline
- 各 presentation instant に対応する camera pose
- camera intrinsics
- 時間的に更新される persistent triangle mesh
- triangle 単位の surface classification
- camera-aligned instantaneous depth
- depth confidence
- scene checkpoint

SSPS v1.0 は画像 pixels, video bitstream, audio, texture, material, light estimate, IMU samples, rolling-shutter trajectory, dynamic-object identity, SLAM relocalization data を格納しない.

HEIF/HEIC および QuickTime/ISO BMFF への格納方法は SSPS 自体の意味論ではなく, 別の container binding specification が定義する.

SSPS の一つの stream は一つの連続した scene coordinate system を持つ. stream 内で scene coordinate system を reset してはならない.

---

## 2. Normative language

本文中の **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT** は必須要件を表す.

**SHOULD**, **SHOULD NOT**, **MAY** は SSPS v1.0 の binary semantics には使用しない. 実装依存の選択を残さないため, v1.0 で規定される wire format と validation rule はすべて MUST / MUST NOT として定義する.

---

## 3. Conformance model

### 3.1 Writer

SSPS v1.0 writer は, 本文書に記載された全ての canonical encoding rule を満たす stream のみを生成しなければならない.

### 3.2 Reader

SSPS v1 reader は以下を実装しなければならない.

1. `version_major == 1` の stream を受理する.
2. `version_minor >= 0` を受理する.
3. 既知の v1 packet を本文書どおりに検証する.
4. 未知かつ non-critical な packet を, payload を解釈せず skip する.
5. 未知かつ critical な packet を `UNSUPPORTED_CRITICAL_PACKET` として拒否する.
6. 構造的に不正な stream を部分的に成功したものとして公開してはならない.

`version_major != 1` は unsupported stream であり, v1 reader は拒否しなければならない.

### 3.3 Validator

validator は本文書の全 MUST / MUST NOT condition を検証しなければならない.

---

## 4. Primitive representations

### 4.1 Byte order

全ての multi-byte integer および floating-point value は **little-endian** で格納する.

### 4.2 Integer types

本文書では以下の型名を用いる.

| Type  | Representation                         |
| ----- | -------------------------------------- |
| `u8`  | unsigned 8-bit integer                 |
| `u16` | unsigned 16-bit integer                |
| `u32` | unsigned 32-bit integer                |
| `u64` | unsigned 64-bit integer                |
| `i32` | two's-complement signed 32-bit integer |

### 4.3 Floating-point

`f32` は IEEE 754 binary32 である.

SSPS v1 writer は NaN および ±Infinity を格納してはならない.

reader は NaN または ±Infinity を含む既知 packet を malformed として拒否しなければならない.

writer は数値的な zero を `+0.0` として encode しなければならない.

reader は `-0.0` を `+0.0` と同一の数値として扱わなければならない.

### 4.4 Boolean representation

SSPS v1 は wire format 上の boolean primitive を持たない. boolean state は bit field または enum value として表現する.

### 4.5 Alignment

SSPS は implicit alignment または compiler padding を持たない.

本文書に記載された field は, 記載された順序で連続して格納する.

C/C++ structure の memory representation をそのまま wire format として書き込んではならない.

---

## 5. Time model

### 5.1 Time unit

全 packet timestamp は **1 nanosecond** 単位の unsigned integer である.

時刻 `t` は次式で seconds に変換される.

$$
 t_{seconds} = \frac{t_{ns}}{1{,}000{,}000{,}000}
$$

### 5.2 Epoch

各 SSPS stream の presentation timeline は `0 ns` から開始する.

負の presentation time は存在しない.

### 5.3 Packet ordering

packet header の `timestamp_ns` は stream 内で non-decreasing でなければならない.

同じ timestamp を持つ packet は Section 20 の ordering rule に従わなければならない.

### 5.4 Scene state at time `t`

persistent geometry state `G(t)` は, `timestamp_ns <= t` の completed checkpoint および geometry update を Section 12 の規則に従って適用した結果である.

camera state は `CAMERA_SAMPLE` が存在する timestamp にのみ定義される.

SSPS v1 semantics は camera pose の temporal interpolation を定義しない.

depth state は `DEPTH_SAMPLE` が存在する timestamp にのみ定義される.

SSPS v1 semantics は depth の temporal interpolation を定義しない.

---

## 6. Coordinate systems

### 6.1 Scene coordinate system

一つの SSPS stream は, stream 全体で不変な右手系 scene coordinate system `S` を一つだけ持つ.

scene coordinate system は最初の `CAMERA_SAMPLE` の camera coordinate system と厳密に一致する.

したがって scene axes は stream 開始時点で以下である.

- `+X`: presentation raster 上の右方向
- `+Y`: presentation raster 上の下方向
- `+Z`: camera optical axis の前方

単位は **meter** である.

scene origin は最初の camera optical center である.

### 6.2 Camera coordinate system

各 `CAMERA_SAMPLE` は右手系 camera coordinate system `C_t` を持つ.

- `+X`: image right
- `+Y`: image down
- `+Z`: optical forward
- origin: camera optical center
- unit: meter

### 6.3 First camera pose

stream 内で最初の `CAMERA_SAMPLE` は `timestamp_ns == 0` でなければならない.

その pose は厳密に identity でなければならない.

- translation = `(0, 0, 0)`
- quaternion = `(0, 0, 0, 1)` in `(x, y, z, w)` order

### 6.4 Camera-to-scene pose

camera pose は camera coordinate から scene coordinate への active rigid transform `T_SC(t)` を表す.

camera-space point `p_C` と scene-space point `p_S` の関係は

$$
 p_S = R(q) p_C + t
$$

である.

inverse は

$$
 p_C = R(q)^T (p_S - t)
$$

である.

quaternion `q = (x, y, z, w)` は Hamilton convention を使用する.

`R(q)` は次式である.

$$
R(q)=
\begin{bmatrix}
1-2(y^2+z^2) & 2(xy-wz) & 2(xz+wy)\\
2(xy+wz) & 1-2(x^2+z^2) & 2(yz-wx)\\
2(xz-wy) & 2(yz+wx) & 1-2(x^2+y^2)
\end{bmatrix}
$$

### 6.5 Quaternion validity and canonical sign

encoded quaternion norm `||q||` は次を満たさなければならない.

$$
|\|q\|-1| \le 10^{-4}
$$

reader は validation 後, 演算時に `q / ||q||` を使用しなければならない.

writer は quaternion sign を以下の規則で canonicalize しなければならない.

1. `w > 0` ならそのまま使用する.
2. `w < 0` なら 4 component 全てに `-1` を乗じる.
3. `w == 0` の場合, `x`, `y`, `z` の順に最初の non-zero component が正になる sign を使用する.
4. identity は `(0, 0, 0, 1)` とする.

---

## 7. Presentation raster and camera model

### 7.1 Presentation raster

SSPS camera intrinsics は container 内の encoded orientation metadata ではなく, 最終的に application が表示・処理する **presentation raster** に対して定義する.

presentation raster は以下を満たす.

- origin: top-left pixel center
- `+u`: right
- `+v`: down
- top-left pixel center: `(0, 0)`
- bottom-right pixel center: `(width - 1, height - 1)`

container binding は, rotation, orientation, clean aperture, crop を解決した後の presentation raster と SSPS intrinsics が一致するようにしなければならない.

### 7.2 Pinhole model

SSPS v1 camera は zero-skew pinhole camera である.

intrinsic matrix `K` は

$$
K=
\begin{bmatrix}
f_x & 0 & c_x\\
0 & f_y & c_y\\
0 & 0 & 1
\end{bmatrix}
$$

である.

camera-space point `(X, Y, Z)` について `Z > 0` の場合, projection は

$$
u = f_x \frac{X}{Z} + c_x
$$

$$
v = f_y \frac{Y}{Z} + c_y
$$

である.

### 7.3 Lens distortion

SSPS v1 は lens distortion parameter を持たない.

writer は, 格納された presentation raster と intrinsics が上記 pinhole model で直接対応する状態に正規化しなければならない.

---

## 8. Stream header

SSPS logical byte stream は 64-byte `StreamHeader` から開始する.

### 8.1 Layout

| Offset | Size | Type  | Field                | Required value              |
| -----: | ---: | ----- | -------------------- | --------------------------- |
|      0 |    8 | bytes | `magic`              | `53 53 50 53 0D 0A 1A 0A`   |
|      8 |    2 | `u16` | `version_major`      | `1`                         |
|     10 |    2 | `u16` | `version_minor`      | `0` for v1.0 writer         |
|     12 |    2 | `u16` | `header_size`        | `64` for v1.0 writer        |
|     14 |    2 | `u16` | `stream_kind`        | Section 8.2                 |
|     16 |    4 | `u32` | `flags`              | `0`                         |
|     20 |    2 | `u16` | `packet_header_size` | `48`                        |
|     22 |    2 | `u16` | `reserved0`          | `0`                         |
|     24 |   16 | bytes | `stream_id`          | non-zero 128-bit identifier |
|     40 |    4 | `u32` | `header_crc32c`      | Section 19                  |
|     44 |   20 | bytes | `reserved1`          | all zero                    |

`stream_id` は各 newly created stream ごとに cryptographically strong random source から生成した 128-bit value とする.

all-zero `stream_id` は invalid である.

### 8.2 Stream kind

| Value | Name    | Meaning                      |
| ----: | ------- | ---------------------------- |
|     1 | `STILL` | single-instant spatial image |
|     2 | `VIDEO` | time-varying spatial media   |

その他の value は v1 reader にとって unsupported である.

### 8.3 Future header extension

`version_major == 1` かつ `header_size > 64` の stream では, v1 reader は first 64 bytes の既知 field を解釈し, remaining header bytes を意味論上無視しなければならない.

header CRC は `header_size` 全体を対象とする.

reader は `64 <= header_size <= 4096` を要求しなければならない. これを外れる StreamHeader は malformed である. major version 1 の future minor version は offsets `0..63` の既存 field, `flags`, `reserved0`, `reserved1` の意味を変更してはならず, 新しい header field は offset `64` 以降にのみ追加しなければならない.

---

## 9. Packet framing

StreamHeader の直後に `PacketHeader + stored payload` が連続する.

SSPS の packet boundary は logical framing boundary である. transport, file I/O, container I/O は任意の byte position で物理的に分割してよいが, reader に渡される logical byte sequence は元の SSPS bytes を欠落・挿入・並べ替えなく再構成しなければならない. container binding が packet 単位の sample 化を行う場合, 一つの SSPS packet を複数の logical metadata sample に分割してはならない.

### 9.1 PacketHeader layout

PacketHeader の v1.0 size は 48 bytes である.

| Offset | Size | Type  | Field                 |
| -----: | ---: | ----- | --------------------- |
|      0 |    4 | bytes | `magic`               |
|      4 |    2 | `u16` | `header_size`         |
|      6 |    2 | `u16` | `type`                |
|      8 |    4 | `u32` | `flags`               |
|     12 |    8 | `u64` | `sequence_number`     |
|     20 |    8 | `u64` | `timestamp_ns`        |
|     28 |    4 | `u32` | `stored_payload_size` |
|     32 |    4 | `u32` | `raw_payload_size`    |
|     36 |    4 | `u32` | `payload_crc32c`      |
|     40 |    4 | `u32` | `header_crc32c`       |
|     44 |    4 | `u32` | `reserved`            |

`magic` は ASCII `SSPK`, bytes `53 53 50 4B` でなければならない.

v1.0 writer は `header_size == 48` としなければならない.

reader は `48 <= header_size <= 4096` を要求しなければならない. `header_size > 48` の場合, first 48 bytes の既知 field を解釈し, remaining header bytes を意味論上無視しなければならない.

StreamHeader の `packet_header_size` は major version 1 における minimum known packet-header prefix size であり, v1.x では `48` のまま変更してはならない. 各 packet の `header_size` は `packet_header_size` 以上でなければならない.

`reserved == 0` でなければならない.

### 9.2 Sequence number

最初の packet は `sequence_number == 0` でなければならない.

以後, 各 packet は直前 packet の `sequence_number + 1` でなければならない.

wraparound は許可しない.

### 9.3 Packet size limits

全 packet は以下を満たさなければならない.

- `stored_payload_size <= 16,777,216`
- `raw_payload_size <= 16,777,216`

reader は上限を超える packet を allocation 前に拒否しなければならない.

### 9.4 Packet flags

| Bit |         Mask | Name             | Meaning                                           |
| --: | -----------: | ---------------- | ------------------------------------------------- |
|   0 | `0x00000001` | `LZ4_COMPRESSED` | stored payload is one raw LZ4 block               |
|   1 | `0x00000002` | `CRITICAL`       | packet semantics are required to interpret stream |

bits 2..31 は v1.0 writer では 0 でなければならない.

既知 v1 packet は Section 10 の required flags と一致しなければ malformed である.

### 9.5 Compression

`LZ4_COMPRESSED == 0` の場合:

- `stored_payload_size == raw_payload_size`
- stored payload bytes が raw payload そのものである.

`LZ4_COMPRESSED == 1` の場合:

- stored payload は **LZ4 raw block format** で圧縮された一つの independent block である.
- dictionary を使用してはならない.
- LZ4 frame header を格納してはならない.
- decompression result size は厳密に `raw_payload_size` と一致しなければならない.

LZ4 block semantics は [LZ4 Block Format Description](https://github.com/lz4/lz4/blob/v1.9.4/doc/lz4_Block_format.md) (2022-07-31 改訂, LZ4 v1.9.4 に収録) に従う.

### 9.6 Unknown packet

reader は unknown packet に対して以下を行う.

- `CRITICAL == 1`: stream を unsupported として拒否する.
- `CRITICAL == 0`: header と stored payload CRC を検証した後, payload を解釈せず skip する.

non-critical unknown packet を skip する際, reader は decompression を行う必要はなく, `raw_payload_size` を allocation に使用してはならない.

---

## 10. Packet type registry

|     Type | Name               | Required flags               |
| -------: | ------------------ | ---------------------------- |
| `0x0001` | `SCENE_INFO`       | `CRITICAL`                   |
| `0x0002` | `CAMERA_SAMPLE`    | `CRITICAL`                   |
| `0x0010` | `CHECKPOINT_BEGIN` | `CRITICAL`                   |
| `0x0011` | `GEOMETRY_PUT`     | `CRITICAL \| LZ4_COMPRESSED` |
| `0x0012` | `GEOMETRY_REMOVE`  | `CRITICAL`                   |
| `0x0013` | `CHECKPOINT_END`   | `CRITICAL`                   |
| `0x0020` | `DEPTH_SAMPLE`     | `CRITICAL \| LZ4_COMPRESSED` |
| `0x00FF` | `STREAM_END`       | `CRITICAL`                   |

`0x0100..0x7FFF` は future SSPS core packet type 用に予約する.

`0x8000..0xFFFE` は vendor extension packet type である.

`0xFFFF` は invalid type であり使用してはならない.

v1.0 writer は上表の standard packet 以外を出力してはならない.

---

## 11. `SCENE_INFO`

### 11.1 Cardinality

stream は `SCENE_INFO` を厳密に一つ含まなければならない.

`SCENE_INFO` は `sequence_number == 0` かつ `timestamp_ns == 0` でなければならない.

### 11.2 Raw payload layout

raw payload size は厳密に 32 bytes である.

| Offset | Size | Type  | Field                   | Required semantics                            |
| -----: | ---: | ----- | ----------------------- | --------------------------------------------- |
|      0 |    4 | `f32` | `gravity_x`             | gravity-down unit vector in scene coordinates |
|      4 |    4 | `f32` | `gravity_y`             | same                                          |
|      8 |    4 | `f32` | `gravity_z`             | same                                          |
|     12 |    4 | `f32` | `reserved_f32`          | `+0.0`                                        |
|     16 |    4 | `u32` | `coordinate_system_id`  | `1`                                           |
|     20 |    4 | `u32` | `geometry_cell_edge_um` | `500000`                                      |
|     24 |    4 | `u32` | `depth_unit_um`         | `1000`                                        |
|     28 |    4 | `u32` | `reserved_u32`          | `0`                                           |

`coordinate_system_id == 1` は Section 6 の scene/camera coordinate convention を表す.

`gravity = (gravity_x, gravity_y, gravity_z)` は **物理的な重力加速度が向く方向**を示す unit vector である.

$$
|\|gravity\|-1| \le 10^{-4}
$$

を満たさなければならない.

---

## 12. Persistent geometry model

### 12.1 Geometry grid

persistent geometry は固定 3D grid cell に分割する.

各 cell edge length は厳密に

$$
L = 0.5\ \text{meter}
$$

である.

cell key は signed integer triple `(cell_x, cell_y, cell_z)` である.

cell origin は

$$
O = L \cdot (cell_x, cell_y, cell_z)
$$

である.

cell spatial domain は各 axis について closed interval `[O_i, O_i + L]` とする.

cell key の comparison は `(cell_x, cell_y, cell_z)` の lexicographic order とする.

### 12.2 Quantized vertices

各 vertex は cell-local unsigned 16-bit triple `(q_x, q_y, q_z)` で表現する.

decode は各 axis について

$$
p_i = O_i + \frac{q_i}{65535} L
$$

である.

writer の quantization は各 axis について

$$
q_i = \operatorname{clamp}_{[0,65535]}
\left(
\left\lfloor
\frac{p_i-O_i}{L} \cdot 65535 + 0.5
\right\rfloor
\right)
$$

でなければならない.

writer は quantization 前の vertex を対応 cell bounds 内に配置しなければならない.

### 12.3 Triangle winding

triangle `(i0, i1, i2)` の geometric normal は

$$
N = \operatorname{normalize}
((v_{i1}-v_{i0}) \times (v_{i2}-v_{i0}))
$$

で定義する.

SSPS v1 は separate normal data を格納しない.

### 12.4 Surface classification

triangle ごとに一つの `u8` classification を持つ.

| Value | Name      |
| ----: | --------- |
|     0 | `UNKNOWN` |
|     1 | `WALL`    |
|     2 | `FLOOR`   |
|     3 | `CEILING` |
|     4 | `TABLE`   |
|     5 | `SEAT`    |
|     6 | `WINDOW`  |
|     7 | `DOOR`    |
|     8 | `OTHER`   |

v1.0 writer は `0..8` のみを出力しなければならない.

reader は `9..254` を `UNKNOWN` として扱わなければならない.

`255` は invalid であり malformed とする.

### 12.5 Geometry state

persistent geometry state は `cell key -> mesh chunk` の finite map である.

同一 state 内に同じ cell key を二つ以上存在させてはならない.

current state の cell count は `262,144` 以下でなければならない.

---

## 13. `GEOMETRY_PUT`

### 13.1 Semantics

`GEOMETRY_PUT` は一つの grid cell の完全な mesh chunk を表す.

checkpoint 外では, 指定 cell が存在しなければ追加し, 存在すれば完全に置換する.

partial vertex update または partial triangle update は存在しない.

### 13.2 Raw payload fixed prefix

raw payload は次の prefix から始まる.

| Offset | Size | Type  | Field            |
| -----: | ---: | ----- | ---------------- |
|      0 |    8 | `u64` | `checkpoint_id`  |
|      8 |    4 | `i32` | `cell_x`         |
|     12 |    4 | `i32` | `cell_y`         |
|     16 |    4 | `i32` | `cell_z`         |
|     20 |    4 | `u32` | `vertex_count`   |
|     24 |    4 | `u32` | `triangle_count` |
|     28 |    4 | `u32` | `reserved`       |

`reserved == 0` でなければならない.

### 13.3 Counts

- `3 <= vertex_count <= 65,535`
- `1 <= triangle_count <= 262,144`

empty cell は `GEOMETRY_PUT` ではなく `GEOMETRY_REMOVE` で表現しなければならない.

### 13.4 Variable arrays

prefix の直後に padding なしで以下を格納する.

1. `vertex_count` 個の quantized vertex
2. `triangle_count` 個の triangle index triple
3. `triangle_count` 個の classification byte

vertex は各 6 bytes:

```text
u16 q_x
u16 q_y
u16 q_z
```

triangle は各 6 bytes:

```text
u16 i0
u16 i1
u16 i2
```

raw payload size は厳密に

$$
32 + 6V + 6T + T
$$

bytes, すなわち

$$
32 + 6V + 7T
$$

でなければならない.

### 13.5 Index validity

全 triangle index は `< vertex_count` でなければならない.

一つの triangle 内の `i0`, `i1`, `i2` は互いに異ならなければならない.

quantization 後に zero-area となる triangle を格納してはならない.

zero-area 判定は decoded vertices について

$$
\| (v_1-v_0) \times (v_2-v_0) \|^2 \le 10^{-16}
$$

を zero-area とする.

### 13.6 Canonical vertex ordering

writer は quantized vertex triple を `(q_x, q_y, q_z)` の lexicographic ascending order に並べなければならない.

同一 quantized vertex triple を重複して格納してはならない.

全 vertex は少なくとも一つの triangle から参照されなければならない.

### 13.7 Canonical triangle ordering

各 triangle は winding を保持したまま cyclic rotation し, 3 index のうち最小値が `i0` になる形にしなければならない.

その後, triangle list 全体を `(i0, i1, i2)` の lexicographic ascending order に並べなければならない.

同一 ordered triangle を重複して格納してはならない.

classification array はこの canonical triangle order と一対一に対応しなければならない.

### 13.8 Checkpoint membership

checkpoint 外の `GEOMETRY_PUT` は `checkpoint_id == 0` でなければならない.

checkpoint 内の `GEOMETRY_PUT` は現在 open 中の checkpoint ID と同じ non-zero `checkpoint_id` を持たなければならない.

---

## 14. `GEOMETRY_REMOVE`

### 14.1 Semantics

`GEOMETRY_REMOVE` は persistent geometry state から一つの cell を削除する.

対象 cell が現在存在しない場合, stream は malformed である.

`GEOMETRY_REMOVE` は checkpoint 内では使用してはならない.

### 14.2 Raw payload

raw payload size は厳密に 16 bytes である.

| Offset | Size | Type  | Field      |
| -----: | ---: | ----- | ---------- |
|      0 |    4 | `i32` | `cell_x`   |
|      4 |    4 | `i32` | `cell_y`   |
|      8 |    4 | `i32` | `cell_z`   |
|     12 |    4 | `u32` | `reserved` |

`reserved == 0` でなければならない.

---

## 15. Checkpoints

### 15.1 Purpose

checkpoint はある timestamp における persistent geometry state 全体を完全に表現する.

checkpoint は delta chain を打ち切り, random seek 時にそれ以前の geometry update を replay する必要をなくす.

### 15.2 `CHECKPOINT_BEGIN`

raw payload size は厳密に 16 bytes である.

| Offset | Size | Type  | Field           |
| -----: | ---: | ----- | --------------- |
|      0 |    8 | `u64` | `checkpoint_id` |
|      8 |    4 | `u32` | `chunk_count`   |
|     12 |    4 | `u32` | `reserved`      |

- `checkpoint_id >= 1`
- first checkpoint ID は `1`
- subsequent checkpoint ID は直前 checkpoint ID + 1
- `chunk_count <= 262,144`
- `reserved == 0`

### 15.3 Checkpoint body

`CHECKPOINT_BEGIN` の直後には, 厳密に `chunk_count` 個の `GEOMETRY_PUT` が連続しなければならない.

それらは全て:

- same `timestamp_ns`
- same `checkpoint_id`
- unique cell key
- cell key lexicographic ascending order

でなければならない.

checkpoint body に他 packet type を interleave してはならない.

### 15.4 `CHECKPOINT_END`

checkpoint body の直後に `CHECKPOINT_END` が存在しなければならない.

raw payload size は厳密に 16 bytes である.

| Offset | Size | Type  | Field           |
| -----: | ---: | ----- | --------------- |
|      0 |    8 | `u64` | `checkpoint_id` |
|      8 |    4 | `u32` | `chunk_count`   |
|     12 |    4 | `u32` | `reserved`      |

`checkpoint_id` と `chunk_count` は対応する begin packet と厳密に一致しなければならない.

`reserved == 0` でなければならない.

### 15.5 Atomicity

reader は checkpoint 全体を staging state に decode しなければならない.

対応する valid `CHECKPOINT_END` を読み終えるまで, checkpoint 内の geometry を externally visible な current scene state として公開してはならない.

`CHECKPOINT_END` の validation 完了時に, previous persistent geometry state を checkpoint body の state で atomic に置換する.

途中で stream が途切れた checkpoint は malformed である.

### 15.6 First checkpoint

stream は first `CAMERA_SAMPLE` より前に, `timestamp_ns == 0` の completed checkpoint を一つ持たなければならない.

first checkpoint は empty geometry を表現する `chunk_count == 0` でも valid である.

### 15.7 Checkpoint cadence

ordinary geometry update とは, checkpoint 外の `GEOMETRY_PUT` または `GEOMETRY_REMOVE` を指す.

writer は同一 timestamp に発生した geometry changes を一つの update batch として評価しなければならない. batch 内で同一 cell key を複数回変更してはならず, batch の `raw_delta_bytes` は, その timestamp に ordinary packet として encode した場合の `GEOMETRY_PUT` / `GEOMETRY_REMOVE` raw payload size の合計とする.

last completed checkpoint 以後について, 次の update batch により以下のどちらかが成立する場合, writer はその batch 適用後の resulting full geometry state を同じ timestamp の新しい checkpoint として encode し, その timestamp の ordinary geometry operation を一切出力してはならない.

1. batch timestamp - last checkpoint timestamp > `30,000,000,000 ns`
2. last checkpoint 以後に実際に出力した ordinary geometry update の raw payload size 合計 + batch `raw_delta_bytes` > `33,554,432 bytes`

この規則により, geometry が変化しない区間では重複 checkpoint を生成しない.

### 15.8 Same-timestamp geometry rule

一つの timestamp に checkpoint と ordinary geometry update を同時に存在させてはならない.

一つの timestamp に同一 cell key への ordinary geometry operation を二つ以上存在させてはならない.

ordinary geometry operation は cell key lexicographic ascending order で格納しなければならない.

---

## 16. `CAMERA_SAMPLE`

### 16.1 Cardinality and timestamps

stream は一つ以上の `CAMERA_SAMPLE` を含まなければならない.

同じ timestamp に二つ以上の `CAMERA_SAMPLE` を置いてはならない.

first `CAMERA_SAMPLE` は `timestamp_ns == 0` でなければならない.

### 16.2 Raw payload layout

raw payload size は厳密に 64 bytes である.

| Offset | Size | Type  | Field           |
| -----: | ---: | ----- | --------------- |
|      0 |    4 | `u32` | `raster_width`  |
|      4 |    4 | `u32` | `raster_height` |
|      8 |    4 | `f32` | `fx`            |
|     12 |    4 | `f32` | `fy`            |
|     16 |    4 | `f32` | `cx`            |
|     20 |    4 | `f32` | `cy`            |
|     24 |    4 | `f32` | `tx`            |
|     28 |    4 | `f32` | `ty`            |
|     32 |    4 | `f32` | `tz`            |
|     36 |    4 | `f32` | `qx`            |
|     40 |    4 | `f32` | `qy`            |
|     44 |    4 | `f32` | `qz`            |
|     48 |    4 | `f32` | `qw`            |
|     52 |   12 | bytes | `reserved`      |

`reserved` は全て zero でなければならない.

### 16.3 Raster validity

- `1 <= raster_width <= 16384`
- `1 <= raster_height <= 16384`

全 `CAMERA_SAMPLE` は stream 内で同一 `raster_width` と `raster_height` を持たなければならない.

### 16.4 Intrinsics validity

- `fx > 0`
- `fy > 0`
- `0 <= cx <= raster_width - 1`
- `0 <= cy <= raster_height - 1`

全値は finite でなければならない.

### 16.5 Pose validity

translation と quaternion component は finite でなければならない.

quaternion は Section 6.5 を満たさなければならない.

first camera sample は Section 6.3 の identity pose と完全に一致しなければならない.

---

## 17. `DEPTH_SAMPLE`

### 17.1 Semantics

`DEPTH_SAMPLE` は対応 camera sample と同一 optical center / pose における camera-aligned instantaneous depth observation である.

SSPS v1 の depth value は camera ray length ではなく **camera-space +Z coordinate** である.

### 17.2 Timestamp relationship

`DEPTH_SAMPLE.timestamp_ns` と同じ timestamp を持つ `CAMERA_SAMPLE` が厳密に一つ存在しなければならない.

同じ timestamp に二つ以上の `DEPTH_SAMPLE` を存在させてはならない.

### 17.3 Raw payload fixed prefix

raw payload は次の 32-byte prefix から始まる.

| Offset | Size | Type  | Field          |
| -----: | ---: | ----- | -------------- |
|      0 |    4 | `u32` | `depth_width`  |
|      4 |    4 | `u32` | `depth_height` |
|      8 |    4 | `f32` | `fx`           |
|     12 |    4 | `f32` | `fy`           |
|     16 |    4 | `f32` | `cx`           |
|     20 |    4 | `f32` | `cy`           |
|     24 |    4 | `u32` | `reserved0`    |
|     28 |    4 | `u32` | `reserved1`    |

`reserved0 == 0` および `reserved1 == 0` でなければならない.

### 17.4 Dimensions

- `1 <= depth_width <= 4096`
- `1 <= depth_height <= 4096`
- `depth_width * depth_height <= 4,194,304`

stream に複数 `DEPTH_SAMPLE` が存在する場合, 全 sample の `depth_width` と `depth_height` は同一でなければならない.

### 17.5 Depth intrinsics

Depth raster の pixel-center convention は Section 7.1 と同一である.

- `fx > 0`
- `fy > 0`
- `0 <= cx <= depth_width - 1`
- `0 <= cy <= depth_height - 1`

全値は finite でなければならない.

pixel `(u, v)` において encoded depth integer `d` が non-zero の場合, camera-space point は

$$
Z = d \cdot 0.001
$$

$$
X = (u-c_x) Z / f_x
$$

$$
Y = (v-c_y) Z / f_y
$$

meter で定義する.

### 17.6 Depth array

prefix の直後に `N = depth_width * depth_height` 個の `u16` depth value を row-major order で格納する.

pixel index は

$$
i = v \cdot depth\_width + u
$$

である.

`d == 0` は invalid / unavailable depth を表す.

`1 <= d <= 65535` は `d` millimeters の valid +Z depth を表す.

writer は 65.535 m を超える depth を `0` として encode しなければならない.

### 17.7 Confidence array

Depth array の直後に `ceil(N / 4)` bytes の confidence data を格納する.

一 pixel あたり 2 bits を使用する.

pixel `i` の confidence は byte `i >> 2` の bit position `2 * (i & 3)` から始まる.

最初の pixel は byte の least-significant 2 bits を使用する.

confidence value:

| Value | Name      |
| ----: | --------- |
|     0 | `INVALID` |
|     1 | `LOW`     |
|     2 | `MEDIUM`  |
|     3 | `HIGH`    |

`depth == 0` の pixel は confidence `0` でなければならない.

`depth != 0` の pixel は confidence `1..3` でなければならない.

最後の confidence byte に未使用 high bits が存在する場合, それらは zero でなければならない.

### 17.8 Raw payload size

raw payload size は厳密に

$$
32 + 2N + \lceil N/4 \rceil
$$

bytes でなければならない.

### 17.9 Relationship to persistent geometry

`DEPTH_SAMPLE` は persistent geometry state を変更しない.

persistent mesh と instantaneous depth の不一致は malformed condition ではない.

SSPS v1 は depth と mesh の fusion rule を定義しない.

---

## 18. `STREAM_END`

### 18.1 Cardinality

stream は `STREAM_END` を厳密に一つ含まなければならない.

`STREAM_END` は stream の最後の packet でなければならない.

### 18.2 Payload

`stored_payload_size == 0` および `raw_payload_size == 0` でなければならない.

`payload_crc32c` は empty byte string の CRC32C, すなわち `0` でなければならない.

### 18.3 Duration

`STREAM_END.timestamp_ns` は logical stream duration を表す.

全 preceding packet は

`packet.timestamp_ns <= STREAM_END.timestamp_ns`

を満たさなければならない.

`STILL` stream では `STREAM_END.timestamp_ns == 0` でなければならない.

`VIDEO` stream では `STREAM_END.timestamp_ns > 0` でなければならない.

---

## 19. CRC32C

SSPS v1 は CRC-32C (Castagnoli) を使用する.

parameter は以下で固定する.

- width: 32
- polynomial: `0x1EDC6F41`
- reflected polynomial: `0x82F63B78`
- initial value: `0xFFFFFFFF`
- input reflection: true
- output reflection: true
- final XOR: `0xFFFFFFFF`

empty byte string の CRC32C は `0x00000000` である.

### 19.1 Stream header CRC

`header_crc32c` 計算時は StreamHeader 内の `header_crc32c` field bytes を zero とみなす.

CRC 対象は `header_size` bytes 全体である.

### 19.2 Packet header CRC

`header_crc32c` 計算時は PacketHeader 内の `header_crc32c` field bytes を zero とみなす.

CRC 対象は `PacketHeader.header_size` bytes 全体である.

### 19.3 Payload CRC

`payload_crc32c` は **stored payload bytes** に対して計算する.

compressed packet では decompressed bytes ではなく LZ4-compressed bytes を対象とする.

reader は payload CRC validation に成功する前に compressed payload を decompress してはならない.

---

## 20. Canonical packet ordering

### 20.1 Global ordering

packet は `timestamp_ns` non-decreasing order でなければならない.

### 20.2 Timestamp zero prefix

`timestamp_ns == 0` の先頭は必ず次の順序である.

1. `SCENE_INFO`
2. first `CHECKPOINT_BEGIN`
3. checkpoint member `GEOMETRY_PUT` packets
4. first `CHECKPOINT_END`
5. `CAMERA_SAMPLE`
6. `DEPTH_SAMPLE` if present at `0`

### 20.3 Later timestamp ordering

各 later timestamp `t` では次の順序を使用する.

1. completed checkpoint block, または ordinary geometry operations のどちらか一方
2. `CAMERA_SAMPLE`
3. `DEPTH_SAMPLE` if present
4. future non-critical extension packets

geometry timestamp には同じ timestamp の `CAMERA_SAMPLE` が存在しなければならない.

### 20.4 Ordinary geometry ordering

ordinary geometry operations は cell key lexicographic order とする.

同一 cell key に一 timestamp 内で複数 operation を置いてはならない.

### 20.5 End packet

`STREAM_END` は最後に一つだけ置く.

---

## 21. Stream-kind invariants

### 21.1 STILL

`stream_kind == STILL` の stream は以下を全て満たさなければならない.

- 全 packet の `timestamp_ns == 0`
- `SCENE_INFO` exactly one
- checkpoint exactly one
- `CAMERA_SAMPLE` exactly one
- ordinary `GEOMETRY_PUT` outside checkpoint: zero
- `GEOMETRY_REMOVE`: zero
- `DEPTH_SAMPLE`: zero or one
- `STREAM_END` exactly one
- first camera pose identity

### 21.2 VIDEO

`stream_kind == VIDEO` の stream は以下を全て満たさなければならない.

- `SCENE_INFO` exactly one at `0`
- first completed checkpoint at `0`
- one or more `CAMERA_SAMPLE`
- first `CAMERA_SAMPLE` at `0`
- first camera pose identity
- `STREAM_END.timestamp_ns > 0`
- every geometry/checkpoint timestamp corresponds to a `CAMERA_SAMPLE` timestamp
- every `DEPTH_SAMPLE` timestamp corresponds to a `CAMERA_SAMPLE` timestamp

---

## 22. Media-binding invariants

SSPS 自体は image/video bytes を持たないが, SpatialSnapshot media container binding は以下を満たさなければならない.

### 22.1 Still image binding

- primary image presentation raster dimensions は `CAMERA_SAMPLE.raster_width/height` と一致する.
- primary image presentation instant は `0 ns` である.

### 22.2 Video binding

- 各 presented video frame の presentation timestamp は SSPS `CAMERA_SAMPLE.timestamp_ns` と一対一対応する.
- 各 SSPS `CAMERA_SAMPLE` は一つの presented video frame に対応する.
- video presentation raster dimensions は全 `CAMERA_SAMPLE.raster_width/height` と一致する.
- SSPS `STREAM_END.timestamp_ns` は bound media の presentation duration と一致する.

container timestamp を nanoseconds に変換する際は nearest integer に round し, exact half は away from zero とする. presentation timeline は non-negative なので half case は常に上方向へ round する.

---

## 23. Geometry update semantics

### 23.1 State transition

checkpoint 外の `GEOMETRY_PUT` at time `t` は `G(t)` における target cell を packet mesh に置換する.

`GEOMETRY_REMOVE` at time `t` は `G(t)` から target cell を削除する.

同じ timestamp の geometry operations は Section 20 の canonical order で全て適用された後, その timestamp の camera/depth sample に対応する geometry state となる.

### 23.2 No object identity

SSPS v1 persistent geometry は surface state のみを表す.

mesh vertex, triangle, cell の temporal object identity を定義しない.

`GEOMETRY_PUT` による cell replacement 前後で vertex index または triangle index の identity を継承してはならない.

### 23.3 No motion field

SSPS v1 は geometry velocity, per-vertex motion, dynamic-object transform を定義しない.

移動物体は instantaneous depth に現れ得るが, persistent geometry update から object identity を推定してはならない.

---

## 24. Deterministic geometric interpretation

### 24.1 Scene mesh reconstruction

ある timestamp `t` における scene mesh は `G(t)` の全 cell mesh の union である.

cell boundaries 上の vertices は複数 cell で同一 coordinates を持ち得る.

reader は異なる cell の vertices を自動 weld してはならない.

### 24.2 Triangle position

triangle vertex position は Section 12.2 の quantization decode の結果をそのまま使用する.

mesh consumer は encoded coordinates を再量子化して scene semantics を変更してはならない.

### 24.3 Classification

classification は triangle attribute であり, vertex attribute ではない.

triangle splitting または derived rendering mesh を作る consumer は元 triangle classification を全 derived primitive に継承しなければならない.

---

## 25. Canonical trim and rebase

SSPS v1 の trim は, 新しい standalone conforming stream を生成する operation として定義する.

### 25.1 Valid trim interval

VIDEO stream に対する trim interval は `[t_start, t_end)` とする.

- `t_start` は existing `CAMERA_SAMPLE.timestamp_ns` と一致しなければならない.
- `t_end` は `t_start < t_end <= original STREAM_END.timestamp_ns` を満たさなければならない.
- `t_end` は existing `CAMERA_SAMPLE.timestamp_ns` または original `STREAM_END.timestamp_ns` と一致しなければならない.

### 25.2 New timeline

output timestamp は

$$
t' = t - t_{start}
$$

とする.

output `STREAM_END.timestamp_ns` は

$$
t_{end} - t_{start}
$$

である.

### 25.3 New scene coordinate system

original scene `S` における camera pose at `t_start` を `T_SC(start)` とする.

new scene `S'` はその camera coordinate system と一致しなければならない.

scene point の変換は

$$
p_{S'} = T_{SC}(start)^{-1} p_S
$$

である.

retained camera pose は

$$
T_{S'C}(t') = T_{SC}(start)^{-1} T_{SC}(t)
$$

とする.

したがって new first camera pose は identity になる.

### 25.4 Gravity transform

original gravity vector `g_S` は translation の影響を受けない.

new gravity は

$$
g_{S'} = R_{SC}(start)^T g_S
$$

である.

normalization 後に `SCENE_INFO` へ格納する.

### 25.5 Geometry at new zero

output は original `G(t_start)` を完全に reconstruct し, Section 25.3 の transform を適用し, new scene grid に再 partition / quantize した state を checkpoint ID `1`, timestamp `0` として出力しなければならない.

original checkpoint packet bytes をそのまま copy してはならない.

### 25.6 Later geometry changes

retained interval 内の original geometry state transition は new scene coordinate system へ変換し, new fixed 0.5 m grid 上の `GEOMETRY_PUT` / `GEOMETRY_REMOVE` として再 encode しなければならない.

original cell key を再利用してはならない.

### 25.7 Depth after trim

Depth value, confidence, depth intrinsics は camera-local data であるため, retained sample では値を変更しない.

timestamp のみ `t_start` を引く.

### 25.8 New stream identity

trim result は新しい non-zero random `stream_id` を持たなければならない.

---

## 26. Concatenation

SSPS v1 は scene reset packet を持たない.

異なる scene coordinate system を持つ二つの stream を byte-level または packet-level に単純連結してはならない.

一つの output SSPS stream は stream 全体を通して一つの scene coordinate system のみを持たなければならない.

---

## 27. Resource and security limits

reader は untrusted input を前提として実装しなければならない.

少なくとも以下を allocation または arithmetic の前に検証しなければならない.

- packet stored/raw size bounds
- integer overflow in payload-size calculation
- depth dimension multiplication overflow
- geometry array size overflow
- triangle index bounds
- checkpoint chunk count
- current scene chunk count
- LZ4 decompressed size
- packet sequence progression
- timestamp ordering
- duplicate cell keys
- duplicate vertex triples
- duplicate canonical triangles
- CRC32C
- finite floating-point values
- quaternion norm

reader は malformed packet に記載された size を信用して unbounded allocation を行ってはならない.

### 27.1 Format-level maxima

| Item                         |     Maximum |
| ---------------------------- | ----------: |
| StreamHeader size            | 4,096 bytes |
| PacketHeader size            | 4,096 bytes |
| stored payload per packet    |      16 MiB |
| raw payload per packet       |      16 MiB |
| current geometry cells       |     262,144 |
| checkpoint chunks            |     262,144 |
| vertices per geometry chunk  |      65,535 |
| triangles per geometry chunk |     262,144 |
| camera raster width/height   | 16,384 each |
| depth raster width/height    |  4,096 each |
| depth pixels per sample      |   4,194,304 |

実装独自の lower resource limit を SSPS semantic limit として扱ってはならない. resource-constrained reader が format-level limit 未満で処理を停止する場合, それは `RESOURCE_LIMIT` として報告し, `MALFORMED` として報告してはならない.

---

## 28. Malformed stream conditions

以下は全て malformed である.

1. invalid StreamHeader magic
2. invalid packet magic
3. failed header CRC
4. failed payload CRC
5. sequence gap / duplication / reordering
6. decreasing timestamp
7. packet size exceeding format limit
8. raw/stored size mismatch for uncompressed packet
9. invalid LZ4 block or decompressed size mismatch
10. invalid required packet flags
11. missing `SCENE_INFO`
12. duplicate `SCENE_INFO`
13. missing first checkpoint
14. unfinished checkpoint
15. checkpoint ID mismatch
16. checkpoint chunk count mismatch
17. checkpoint member interleaving
18. duplicate checkpoint cell key
19. geometry update referencing invalid payload
20. geometry removal of absent cell
21. invalid geometry index
22. zero-area triangle
23. duplicate quantized vertex
24. unreferenced vertex
25. duplicate canonical triangle
26. invalid classification `255`
27. invalid camera raster dimensions
28. invalid intrinsics
29. non-finite camera values
30. invalid quaternion norm
31. non-identity first camera pose
32. depth sample without matching camera timestamp
33. duplicate depth sample at same timestamp
34. invalid depth/confidence relationship
35. invalid depth payload size
36. packet ordering violation
37. missing `STREAM_END`
38. duplicate `STREAM_END`
39. bytes or packets after `STREAM_END`
40. STILL invariant violation
41. VIDEO invariant violation

---

## 29. Unsupported stream conditions

以下は malformed ではなく unsupported である.

1. `version_major != 1`
2. unknown `stream_kind`
3. unknown packet with `CRITICAL` flag

reader API は malformed と unsupported を区別して報告しなければならない.

---

## 30. Streaming decoder state machine

reader は少なくとも以下の logical states を区別しなければならない.

```text
EXPECT_SCENE_INFO
EXPECT_FIRST_CHECKPOINT
IN_CHECKPOINT
EXPECT_FIRST_CAMERA
RUNNING
ENDED
```

### 30.1 Transitions

```text
EXPECT_SCENE_INFO
  SCENE_INFO -> EXPECT_FIRST_CHECKPOINT

EXPECT_FIRST_CHECKPOINT
  CHECKPOINT_BEGIN(id=1,t=0) -> IN_CHECKPOINT

IN_CHECKPOINT
  GEOMETRY_PUT(checkpoint member) -> IN_CHECKPOINT
  CHECKPOINT_END -> EXPECT_FIRST_CAMERA or RUNNING

EXPECT_FIRST_CAMERA
  CAMERA_SAMPLE(t=0, identity) -> RUNNING

RUNNING
  CHECKPOINT_BEGIN -> IN_CHECKPOINT
  GEOMETRY_PUT -> RUNNING
  GEOMETRY_REMOVE -> RUNNING
  CAMERA_SAMPLE -> RUNNING
  DEPTH_SAMPLE -> RUNNING
  unknown non-critical -> RUNNING
  STREAM_END -> ENDED

ENDED
  any further byte/packet -> MALFORMED
```

checkpoint entered from `RUNNING` は完了後 `RUNNING` に戻る.

reader は state transition に違反する known packet を malformed として拒否しなければならない.

---

## 31. Minimal conforming STILL stream

最小 valid STILL stream の logical packet sequence は厳密に次である.

```text
StreamHeader(kind=STILL)

seq=0  t=0  SCENE_INFO
seq=1  t=0  CHECKPOINT_BEGIN(id=1, chunk_count=0)
seq=2  t=0  CHECKPOINT_END(id=1, chunk_count=0)
seq=3  t=0  CAMERA_SAMPLE(identity)
seq=4  t=0  STREAM_END
```

LiDAR depth を含む STILL では `DEPTH_SAMPLE` を `CAMERA_SAMPLE` と `STREAM_END` の間に一つ置く.

---

## 32. Versioning rules

SSPS file-format version は Semantic Versioning ではない.

- `major` change: v1 reader が正しく解釈できない core semantic change
- `minor` change: v1 reader が unknown-packet rule に従って安全に処理できる additive change

v1.x で既存 v1 packet の raw payload layout を変更してはならない.

v1.x で既存 field の意味を変更してはならない.

v1.x の additive extension は new packet type として追加しなければならない.

既存 v1 reader にとって必須となる new packet は `CRITICAL` を設定しなければならない.

既存 v1 reader が無視しても既知 semantics を誤解しない packet のみ non-critical として定義できる.

---

## 33. What SSPS v1 deliberately does not encode

SSPS v1 では以下の semantics は存在しない.

- image/video pixel payload
- audio payload
- lens-distortion coefficients
- rolling-shutter per-scanline pose
- IMU sample stream
- absolute geographic location
- AR session proprietary world map
- object identity
- skeletal tracking
- dynamic object trajectories
- materials
- texture coordinates
- textures
- vertex normals
- vertex colors
- lighting model
- shadows
- environment probes
- camera exposure metadata
- semantic segmentation masks
- mesh acceleration structures

これらの情報が欠落していても v1 stream は incomplete ではない.

v1 reader は上記 semantics を SSPS v1 data から暗黙に存在すると仮定してはならない.

---

## 34. Normative external references

SSPS v1.0 が外部仕様へ依存するのは以下のみである.

1. IEEE 754 binary32 floating-point representation
2. CRC-32C / Castagnoli polynomial
3. [LZ4 Block Format Description](https://github.com/lz4/lz4/blob/v1.9.4/doc/lz4_Block_format.md), 2022-07-31 改訂 (LZ4 v1.9.4 に収録)

container-specific specification は SSPS core format の normative dependency ではない.
