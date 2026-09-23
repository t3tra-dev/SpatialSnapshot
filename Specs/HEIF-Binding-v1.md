# SpatialSnapshot HEIF Binding Version 1.0

**Status:** Normative specification
**File:** `Specs/HEIF-Binding-v1.md`

---

## 1. Scope

SpatialSnapshot HEIF Binding v1.0 は, 一つの `SSPS` `STILL` stream を, 一つの HEIF primary image に関連付けて格納する方法を定義する.

本仕様は以下を定義する.

- SpatialSnapshot を含む HEIF file の識別方法
- SSPS metadata item の宣言
- SSPS metadata item と primary image item の関連付け
- SSPS byte stream の格納方法
- primary image presentation raster と SSPS camera raster の一致条件
- orientation, crop, pixel aspect ratio に関する制約
- item location と extent に関する制約
- reader / writer / validator の conformance rule
- editing, copy, metadata stripping 時の整合性規則
- malformed / unsupported condition
- resource and security limit

本仕様は画像 codec の bitstream syntax, color management, Exif syntax, XMP syntax, auxiliary image syntax, thumbnail syntaxを再定義しない.

本仕様に conform する file は, SSPS metadata を除去した場合にも通常の HEIF image として解釈可能でなければならない.

---

## 2. Normative language

本文中の **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT** は必須要件を表す.

SpatialSnapshot HEIF Binding v1.0 の binding semantics に `SHOULD`, `SHOULD NOT`, `MAY` は使用しない.

下位の HEIF / ISO BMFF specification が複数の合法表現を持つ場合, 本仕様がその表現を一意に制限していない限り, その差は SpatialSnapshot binding semantics に影響しない.

---

## 3. Normative dependencies

SpatialSnapshot HEIF Binding v1.0 は以下を前提とする.

1. `SSPS/1.0` および compatible `SSPS/1.x`
2. ISO/IEC 14496-12:2026, ISO Base Media File Format
3. ISO/IEC 23008-12:2025, Image File Format (HEIF)

上位仕様間で用語が異なる場合, 本仕様では ISO BMFF の `box` と QuickTime の `atom` を区別し, HEIF については `box` と呼ぶ.

---

## 4. Conformance classes

### 4.1 Binding-conforming writer

writer は本文書の全 writer requirement を満たす HEIF file のみを SpatialSnapshot HEIF v1 として出力しなければならない.

### 4.2 Binding-conforming reader

reader は本文書の discovery algorithm に従って SSPS item を発見し, binding-level validation に成功した場合にのみ SpatialSnapshot data を公開しなければならない.

### 4.3 Binding-conforming validator

validator は以下を独立に報告しなければならない.

- host HEIF structural failure
- SpatialSnapshot binding malformed condition
- SSPS malformed condition
- unsupported SSPS major version
- unsupported critical SSPS extension

一つを他の failure class として誤報してはならない.

---

## 5. Terminology

### 5.1 Host file

SpatialSnapshot metadata を含む HEIF file 全体を **host file** と呼ぶ.

### 5.2 Primary image

root-level HEIF `meta` box の `pitm` が指す item を **primary image item** と呼ぶ.

### 5.3 SSPS item

Section 9 の宣言条件を満たし, Section 10 の `cdsc` reference によって primary image item を記述する metadata item を **SSPS item** と呼ぶ.

### 5.4 Presentation raster

primary image を SpatialSnapshot application が camera image として解釈する最終 raster を **presentation raster** と呼ぶ.

SpatialSnapshot HEIF v1 では presentation raster と primary image の decoded raster は Section 13 の規則により同一である.

---

## 6. Host HEIF requirements

host file は valid HEIF file でなければならない.

host file は少なくとも以下を含まなければならない.

```text
ftyp
meta
  hdlr
  pitm
  iloc
  iinf
  iref
  ...
mdat
```

`meta` は file-level root metadata box でなければならない.

root `meta/hdlr.handler_type` は ASCII FourCC `pict` でなければならない.

`pitm` は exactly one primary image item を指定しなければならない.

host file は image sequence を SpatialSnapshot STILL の primary representation として使用してはならない.

---

## 7. SpatialSnapshot binding cardinality

一つの SpatialSnapshot HEIF v1 host file は, primary image item に対して exactly one SSPS item を持たなければならない.

以下は malformed binding である.

- SSPS item が zero
- primary image を記述する SSPS item が二つ以上
- 一つの SSPS item が複数 image item を `cdsc` target とする
- SSPS item が primary image 以外のみを `cdsc` target とする

primary image 以外の image item, thumbnail, auxiliary image, Exif, XMP, その他 metadata item は host HEIF specification に従って存在できる. それらは SSPS item として解釈してはならない.

---

## 8. Primary image item

### 8.1 Item kind

primary image item は HEIF において presentation 可能な image item でなければならない.

primary image item の `item_type` は, valid coded image item type または ASCII FourCC `grid` のいずれかでなければならない.

`grid` primary image の `dimg` targets はすべて valid coded image items でなければならず, derived image item を tile target として参照してはならない.

image overlay, identity derived image, その他 `grid` 以外の derived image item を primary image として使用してはならない.

`grid` は coded tiles を一つの final presentation raster へ構成する storage representation としてのみ扱い, SpatialSnapshot coordinate system に追加 transform を導入しない.

`grid` primary image の各 `dimg` target には `irot`, `imir`, `clap`, `rloc` を関連付けてはならない. 各 tile の pixel aspect ratio は square でなければならない. これにより `grid` は tile ordering と concatenation のみを presentation geometry に寄与させる.

### 8.2 Spatial extent

primary image item は exactly one associated `ispe` property を持たなければならない.

`ispe.image_width` および `ispe.image_height` は non-zero でなければならない.

その値は Section 13 に従い SSPS `CAMERA_SAMPLE.raster_width` / `raster_height` と厳密に一致しなければならない.

### 8.3 Image codec

SpatialSnapshot binding は primary image codec の payload semantics を変更しない.

coded primary image または `grid` を構成する各 coded tile の codec は, その host file の `ftyp` brand, item type, decoder configuration property と整合する valid HEIF coded image でなければならない.

reader は SSPS metadata を抽出するために image bitstream を decode することを要求されない.

---

## 9. SSPS metadata item declaration

### 9.1 Item type

SSPS item の `ItemInfoEntry` (`infe`) は以下を満たさなければならない.

| Field                   | Required value                                      |
| ----------------------- | --------------------------------------------------- |
| `item_type`             | ASCII FourCC `mime`                                 |
| `item_name`             | UTF-8 string `SpatialSnapshot`                      |
| `content_type`          | ASCII string `application/vnd.spatialsnapshot.ssps` |
| `content_encoding`      | empty string                                        |
| `item_protection_index` | `0`                                                 |

strings は host HEIF / ISO BMFF が `infe` に対して定める terminating NUL を含めて格納する.

`content_type` comparison は byte-for-byte case-sensitive comparison とする.

parameter 付き MIME value は v1 SSPS item として認識してはならない.

### 9.2 Hidden flag

SSPS metadata item は metadata item として image presentation 対象ではない.

`infe` の hidden item flag が定義される version を用いる場合, その flag は `0` でなければならない.

### 9.3 Protection

SSPS item は item protection を使用してはならない.

`ipro` / protection scheme によって SSPS item payload を暗号化, wrap, transform してはならない.

---

## 10. SSPS-to-image relationship

### 10.1 Reference type

SSPS item は `iref` 内の item reference type `cdsc` によって primary image item を記述しなければならない.

reference direction は厳密に以下である.

```text
SSPS metadata item --cdsc--> primary image item
```

逆方向の reference は SpatialSnapshot binding を表さない.

### 10.2 Target count

SSPS item からの `cdsc` target count は exactly `1` でなければならない.

その target item ID は `pitm` が示す primary image item ID と厳密に一致しなければならない.

### 10.3 Additional references

SSPS item は v1 writer により `cdsc` 以外の outgoing item reference を持ってはならない.

primary image item は SSPS item 以外の metadata / thumbnail / auxiliary relationship を持つことができる.

---

## 11. SSPS item location

### 11.1 Location mechanism

SSPS item data は `iloc` から参照されなければならない.

SSPS item の location は以下を全て満たさなければならない.

- `data_reference_index == 0`
- construction method は file offset based method `0`
- extent count は exactly `1`
- extent length は non-zero
- extent は file 内に完全に収まる
- extent は一つの top-level `mdat` box payload 内に完全に収まる

SSPS item data を `idat`, external data reference, 複数 extent, derived item construction により格納してはならない.

### 11.2 Extent length

SSPS item extent length は SSPS logical byte stream の byte length と厳密に一致しなければならない.

extent の先頭 byte は SSPS `StreamHeader.magic` の先頭 `0x53` でなければならない.

extent の末尾 byte は final `STREAM_END` packet の stored representation の末尾でなければならない.

SSPS extent の前後に binding-specific prefix, suffix, padding, length field, Base64, JSON, CBOR, compression wrapper を加えてはならない.

---

## 12. SSPS payload semantics

### 12.1 Exact stream representation

SSPS item payload は SSPS logical stream を byte-for-byte 格納する.

layout は以下である.

```text
SSPS StreamHeader
SSPS PacketHeader + stored payload
SSPS PacketHeader + stored payload
...
SSPS STREAM_END packet
```

SSPS 内部の little-endian representation は変更してはならない.

HEIF / ISO BMFF の big-endian integer representation と SSPS 内部 representation を混同してはならない.

### 12.2 Required stream kind

SSPS item の `StreamHeader.stream_kind` は `STILL (1)` でなければならない.

`VIDEO (2)` を HEIF STILL binding に格納した file は malformed binding である.

### 12.3 SSPS time invariant

bound SSPS は `SSPS/1.x` STILL invariant を満たさなければならない.

したがって全 SSPS packet timestamp は `0 ns` であり, exactly one `CAMERA_SAMPLE` を持つ.

---

## 13. Presentation raster binding

### 13.1 Required equality

SSPS `CAMERA_SAMPLE` の

```text
raster_width
raster_height
```

は primary image presentation raster dimensions と厳密に一致しなければならない.

### 13.2 Container transform prohibition

primary image item に以下の transform を適用してはならない.

- `irot`
- `imir`
- `clap` that changes presentation extent
- `rloc`

primary image decoded raster がそのまま SpatialSnapshot presentation raster でなければならない.

### 13.3 Pixel aspect ratio

primary image item に `pasp` property が存在しない場合, pixel aspect ratio は `1:1` と解釈する.

`pasp` が存在する場合, `hSpacing == vSpacing` でなければならない.

non-square presentation pixel を持つ primary image は malformed binding である.

### 13.4 Spatial extent

primary image `ispe` dimensions は final presentation raster dimensions を表し, coded image と `grid` のどちらでも SSPS raster dimensions と直接比較する.

---

## 14. Exif orientation interaction

Exif metadata item が primary image を記述する場合, Exif Orientation tag が存在するときその値は `1` でなければならない.

Exif Orientation tag が absent の場合は binding 上 `1` と同等に扱う.

values `2..8` を持つ Exif Orientation と SSPS metadata が共存する file は malformed binding である.

reader は SSPS projection を Exif Orientation によって追加変換してはならない.

---

## 15. XMP and other metadata interaction

XMP metadata が primary image を記述し, XMP TIFF namespace の `Orientation` property を含む場合, その整数値は `1` でなければならない. values `2..8` は malformed binding である.

XMP, C2PA, JUMBF, maker metadata その他の metadata は SpatialSnapshot binding の coordinate semantics を上書きしない. binding-conforming reader はそれらから以下を再定義してはならない.

- SSPS coordinate system
- SSPS camera intrinsics
- SSPS presentation raster orientation
- SSPS presentation raster crop
- SSPS stream identity

XMP その他の非-binding metadata が編集指示, crop, rotation, mirror, warp 等を記述していても, それは SpatialSnapshot presentation raster には適用されない. application がその編集結果を新しい primary presentation raster として materialize する場合, Section 22.3 の raster-changing edit rule を適用しなければならない.

---

## 16. Auxiliary and thumbnail images

thumbnail または auxiliary image は host file に存在できる.

それらは以下を満たさなければならない.

- SSPS item の `cdsc` target ではない
- primary image item を置き換える SpatialSnapshot raster として解釈されない
- SSPS `DEPTH_SAMPLE` の substitute として暗黙に利用されない

HEIF depth auxiliary image が存在しても, SSPS depth semantics は SSPS item 内の `DEPTH_SAMPLE` のみによって定義される.

---

## 17. Item identifiers and box versions

SpatialSnapshot binding は item identifier の数値自体に意味を与えない.

reader は `pitm`, `iinf/infe`, `iloc`, `iref` の version に従い item ID width を正しく処理しなければならない.

writer は host file の全 item ID を一意にしなければならない.

SSPS item ID を magic value, FourCC の integer value, stream ID の一部として解釈してはならない.

binding identity は Section 9 の MIME declaration と Section 10 の `cdsc` relationship によってのみ決定する.

---

## 18. File brands

SpatialSnapshot HEIF v1 は private file brand を定義しない.

writer は画像 codec および HEIF host profile に対して正しい `ftyp` major brand / compatible brand を出力しなければならない.

SpatialSnapshot presence を `ftyp` brand のみによって判定してはならない.

reader は Section 25 の discovery algorithm を使用しなければならない.

---

## 19. Box ordering

SpatialSnapshot binding は HEIF / ISO BMFF が box order を規定する箇所以外に追加の top-level box ordering を課さない.

ただし SSPS item extent は Section 11 により一つの `mdat` payload 内へ格納されなければならない.

SSPS reader は `meta` が `mdat` より前にあること, または `mdat` が `meta` より前にあることを前提としてはならない.

---

## 20. SSPS extent overlap

SSPS item extent は以下と byte range overlap してはならない.

- primary image item extent
- Exif item extent
- XMP item extent
- other metadata item extent
- another image item extent
- ISO BMFF box header bytes

同じ underlying bytes を複数 item が完全に共有する deduplication も SSPS item には使用してはならない.

---

## 21. Integrity model

HEIF binding 自体は追加 checksum を定義しない.

SSPS integrity は以下で検証する.

1. `StreamHeader.header_crc32c`
2. 各 packet `header_crc32c`
3. 各 packet `payload_crc32c`

reader は host box parsing と extent bounds validation に成功した後に SSPS CRC validation を行わなければならない.

binding layer は SSPS CRC failure を host HEIF parse failure として報告してはならない.

---

## 22. Preservation and rewrite semantics

### 22.1 Lossless container rewrite

primary image decoded raster と SSPS item payload を変更しない container rewrite は, SSPS item, its `iloc` extent, `infe` declaration, `cdsc` relationship を新しい container layout に整合するよう再記述しなければならない.

byte offset が変化した場合, old `iloc` offset を保持してはならない.

### 22.2 Image re-encode without geometric change

primary image を再 encode しても presentation raster dimensions, pixel coordinate system, pixel orientation が変化しない場合, SSPS item payload を変更してはならない.

### 22.3 Raster-changing edit

以下の edit を行った結果を既存 SSPS item と組み合わせて SpatialSnapshot HEIF v1 として出力してはならない.

- crop
- rotate
- mirror
- perspective warp
- resize
- non-uniform scale
- lens distortion transform
- pixel-coordinate remap

これらを行う editor は SSPS camera intrinsics および必要な geometry/depth semantics を仕様に従い完全に再生成するか, SSPS item と `cdsc` relationship を削除しなければならない.

v1 binding は raster-changing edit に対する metadata-only patch algorithm を定義しない.

### 22.4 Metadata stripping

SSPS metadata を削除する場合, writer は以下を全て削除しなければならない.

- SSPS `infe` entry
- SSPS `iloc` entry
- SSPS item からの `cdsc` reference
- SSPS extent bytes が専有する storage またはそれを unreachable data として残さない再構築

primary image 自体は通常の HEIF image として保持できる.

---

## 23. Copy semantics

SpatialSnapshot-aware item copy は SSPS item 単独を primary image から分離して copy してはならない.

SSPS item を別 host file へ copy する場合, copy target の primary image presentation raster が元 image と byte-level または semantically identical な camera raster であり, 同一 capture を表す場合にのみ SSPS payload を再利用できる.

別 capture image へ SSPS payload を移植してはならない.

`stream_id` は capture identity の一部であり, 単純 container rewrite では変更してはならない.

---

## 24. Still-image lifecycle

SpatialSnapshot HEIF v1 は one-instant media である.

SSPS canonical trim operation は HEIF STILL binding に適用しない.

primary image の presentation instant は `0 ns` である.

SSPS `STREAM_END.timestamp_ns` は `0` でなければならない.

---

## 25. Reader discovery algorithm

binding-conforming reader は以下の手順で SSPS item を発見しなければならない.

1. host file を HEIF / ISO BMFF として parse する.
2. root-level `meta` を locate し, `meta/hdlr.handler_type == 'pict'` を確認する.
3. `pitm` から primary image item ID `P` を取得する.
4. `iinf` の全 item info entry を列挙する.
5. Section 9.1 の `item_type`, `item_name`, `content_type`, `content_encoding`, protection condition を全て満たす item の集合を `S` とする.
6. `iref` を解析し, 各 `s in S` について outgoing `cdsc` target list を得る.
7. target list が exactly `[P]` である item のみ残す.
8. 残る item が exactly one でなければ binding failure とする.
9. `iloc` からその item の storage location を解決する.
10. Section 11 / 20 の extent condition を検証する.
11. extent bytes を SSPS logical stream として parse する.
12. SSPS `stream_kind == STILL` を検証する.
13. Section 13 / 14 の presentation raster condition を検証する.
14. SSPS validator により stream 全体を検証する.
15. 全 validation 成功後にのみ SpatialSnapshot scene を公開する.

reader は file 全体を substring search して SSPS magic を探索してはならない.

reader は MIME item があっても `cdsc` target が primary image でなければ SpatialSnapshot binding として扱ってはならない.

---

## 26. Writer construction algorithm

binding-conforming writer は以下の順序で binding を生成しなければならない.

1. final primary presentation raster を確定する.
2. container-level rotation / mirror / crop が不要な orientation に primary image pixels を生成する.
3. SSPS STILL stream をその raster に対して生成する.
4. SSPS stream を完全 validation する.
5. Section 9 の MIME metadata item を一つ作成する.
6. SSPS bytes を一つの contiguous extent として `mdat` payload に格納する.
7. `iloc` に file-offset construction method `0`, one extent として登録する.
8. SSPS metadata item から primary image item へ one `cdsc` reference を登録する.
9. `pitm`, `ispe`, orientation constraints を再検証する.
10. final file byte offsets を確定後 `iloc` を確定する.
11. 完成 file を Section 25 の reader algorithm と同じ規則で再検証する.

writer は未検証 SSPS stream を SpatialSnapshot metadata item として commit してはならない.

---

## 27. Random access

HEIF STILL binding の SSPS item は one contiguous extent であるため, reader は extent offset と length の二値のみで SSPS stream 全体へアクセスできる.

binding v1 は SSPS item 内の secondary index を定義しない.

SSPS STILL は one timestamp のため container-level temporal seek を持たない.

---

## 28. Resource and security limits

reader は untrusted host file を前提としなければならない.

binding-conforming standard implementation は少なくとも以下の hard limit を適用しなければならない.

```text
HEIF_BINDING_MAX_FILE_BYTES        = 34_359_738_368   // 32 GiB
HEIF_BINDING_MAX_ITEM_COUNT        = 65_536
HEIF_BINDING_MAX_REFERENCE_COUNT   = 1_048_576
HEIF_BINDING_MAX_BOX_DEPTH         = 64
HEIF_BINDING_MAX_BOX_COUNT         = 1_048_576
HEIF_BINDING_MAX_SSPS_BYTES        = 2_147_483_648    // 2 GiB
```

`SSPS item extent_length > HEIF_BINDING_MAX_SSPS_BYTES` は standard implementation における resource-limit failure である.

reader は以下の arithmetic を checked integer arithmetic で行わなければならない.

- box `offset + size`
- `base_offset + extent_offset`
- extent start + extent length
- item count allocation
- reference count allocation

integer overflow を file truncation と同一視してはならない.

---

## 29. Malformed binding conditions

以下のいずれかは SpatialSnapshot HEIF v1 malformed binding である.

1. root `meta/hdlr.handler_type` が `pict` でない, または primary image item が存在しない.
2. primary image が coded image item または `grid` 以外である, または `grid` が derived tile target を含む.
3. `grid` primary image の tile に Section 8.1 が禁止する transform または non-square pixel aspect ratio がある.
4. Section 9 の SSPS MIME item が primary image に zero または multiple 存在する.
5. SSPS item content type が完全一致しない.
6. SSPS item content encoding が non-empty である.
7. SSPS item が protected である.
8. SSPS `cdsc` direction が逆である.
9. SSPS item が複数 `cdsc` target を持つ.
10. `cdsc` target が `pitm` primary item と一致しない.
11. SSPS item が external data reference を使用する.
12. SSPS item が construction method `0` 以外を使用する.
13. SSPS item が複数 extent を使用する.
14. SSPS extent が一つの `mdat` payload 内に完全に収まらない.
15. SSPS extent が他 item extent と overlap する.
16. SSPS payload が exact SSPS stream でない.
17. SSPS stream kind が `STILL` でない.
18. SSPS raster dimensions と primary image raster dimensions が不一致である.
19. primary image に non-identity `irot` / `imir` がある.
20. primary image clean aperture が raster extent を変更する.
21. primary image pixel aspect ratio が non-square である.
22. Exif Orientation が `1` 以外である.
23. SSPS validator が malformed stream と判定する.
24. SSPS item extent size と logical SSPS stream size が一致しない.
25. item ID / box offset / extent arithmetic が overflow する.
26. file truncation により declared box / extent が読めない.
27. primary image を記述する XMP `Orientation` が `1` 以外である.
28. SSPS `infe.item_name` が UTF-8 `SpatialSnapshot` と byte-for-byte 一致しない.

standard validator は, 安全な解析継続が不可能な host-structure failure を除き, 検出可能な独立した malformed condition を列挙して報告しなければならない.

## 30. Unsupported conditions

以下は malformed ではなく unsupported condition とする.

- SSPS `version_major != 1`
- SSPS v1 reader が理解しない critical packet
- host image codec を application が decode できないが, HEIF structure と SpatialSnapshot metadata は valid である場合

image codec unsupported は SSPS extraction 自体を失敗させてはならない.

SSPS scene data は image decode availability と独立して検証可能である.

---

## 31. Unknown HEIF structures

HEIF / ISO BMFF が unknown box / property / metadata item を safely skip する規則を持つ場合, SpatialSnapshot reader は host specification の rule に従わなければならない.

unknown host metadata の存在のみを理由に SpatialSnapshot binding を拒否してはならない.

ただし unknown structure が primary image presentation raster の解釈を変更する essential image property として関連付けられている場合, その primary image は SpatialSnapshot v1 presentation raster と一意に対応しないため unsupported binding とする.

---

## 32. Binding identity

SpatialSnapshot HEIF v1 の存在は以下の conjunction により定義する.

```text
item_type      == 'mime'
content_type   == 'application/vnd.spatialsnapshot.ssps'
content_encoding == ''
outgoing cdsc target == primary item
valid SSPS payload
```

file extension `.heic` / `.heif`, UTType, file brand, Exif tag, XMP property は binding identity の一部ではない.

---

## 33. File extension and MIME transport

SpatialSnapshot binding は host HEIF の通常の file extension / media type を変更しない.

HEVC-coded HEIF file は通常の HEIC transport type として扱われる.

SpatialSnapshot metadata が存在することを理由に独自 file extension を要求してはならない.

application-level feature detection は Section 25 の discovery algorithm によって行う.

---

## 34. Editing transaction requirements

SpatialSnapshot-aware editor は file update を atomic logical transaction として扱わなければならない.

次の状態を persistent output として残してはならない.

- new image + old SSPS
- old image + new SSPS
- new SSPS extent + stale `iloc`
- new SSPS item + missing `cdsc`
- removed SSPS item + dangling `cdsc`

write failure 時は pre-edit valid file または no output のいずれかでなければならない.

---

## 35. Decoder exposure rule

reader は binding validation と SSPS full validation が完了する前に以下を public API から公開してはならない.

- camera intrinsics
- mesh
- depth
- scene gravity
- raycastable scene

partial parse data は diagnostics にのみ使用でき, valid SpatialSnapshot scene として扱ってはならない.

---

## 36. Versioning

SpatialSnapshot HEIF Binding version は SSPS version と独立する.

`SpatialSnapshot-HEIF/1.x` は以下を変更してはならない.

- MIME content type
- `cdsc` direction
- one-item cardinality
- exact SSPS payload rule
- presentation raster identity rule
- `iloc` one-extent / construction-method-0 rule

binding major version の変更は, v1 reader が同じ item graph を正しく解釈できなくなる変更に対してのみ行う.

SSPS v1 minor extension は, その SSPS stream が SSPS v1 reader の forward-compatibility rule を満たす限り HEIF Binding v1 に格納できる.

---

## 37. Normative external references

1. **ISO/IEC 14496-12:2026**, Information technology — Coding of audio-visual objects — Part 12: ISO base media file format.
2. **ISO/IEC 23008-12:2025**, Information technology — High efficiency coding and media delivery in heterogeneous environments — Part 12: Image File Format.
3. **SpatialSnapshot Packet Stream (SSPS) Version 1.0**, `Specs/SSPS-v1.md`.

---

## 38. Error classification

standard implementation は少なくとも以下の error domain を区別しなければならない.

```text
HEIF_MALFORMED
BINDING_MALFORMED
SSPS_MALFORMED
UNSUPPORTED_BINDING
UNSUPPORTED_SSPS
RESOURCE_LIMIT
IMAGE_DECODER_UNAVAILABLE
IO_ERROR
```

各 domain の意味は以下である.

- `HEIF_MALFORMED`: SpatialSnapshot の有無とは独立した host HEIF / ISO BMFF structure が invalid.
- `BINDING_MALFORMED`: host structure は parse できるが SpatialSnapshot HEIF v1 binding rule に違反.
- `SSPS_MALFORMED`: extracted SSPS が SSPS v1 syntax / invariant に違反.
- `UNSUPPORTED_BINDING`: future HEIF property / item construct の semantics を安全に決定できず, presentation raster または binding identity を一意に解釈できない.
- `UNSUPPORTED_SSPS`: SSPS major version または critical extension が未対応.
- `RESOURCE_LIMIT`: syntax 上は表現可能だが standard implementation の Section 28 hard limit を超える.
- `IMAGE_DECODER_UNAVAILABLE`: spatial metadata は valid だが requested decoded-image operation に必要な image decoder が利用不能.
- `IO_ERROR`: file bytes の取得・永続化に失敗.

image decoder の欠如を `UNSUPPORTED_BINDING` または `SSPS_MALFORMED` として報告してはならない.

一つの error が後続 validation を安全に継続不能にした場合, validator はその dependency に依存する検査を skipped として記録できる. ただし既に独立して検出可能な error を一つに潰して報告してはならない.
