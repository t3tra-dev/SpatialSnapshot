# SpatialSnapshot QuickTime Binding Version 1.0

**Status:** Normative specification
**File:** `Specs/QuickTime-Binding-v1.md`

---

## 1. Scope

SpatialSnapshot QuickTime Binding v1.0 は, 一つの `SSPS` `VIDEO` stream を, 一つの QuickTime Movie の visual presentation track に同期した timed metadata track として格納する方法を定義する.

本仕様は以下を定義する.

- SpatialSnapshot を含む QuickTime Movie の識別方法
- SpatialSnapshot timed metadata track の宣言
- bound video track と metadata track の `cdsc` relationship
- `mebx` timed metadata sample description
- SpatialSnapshot metadata key と raw-data datatype
- SSPS logical byte stream の metadata sample への分割規則
- video presentation timestamp と SSPS `CAMERA_SAMPLE.timestamp_ns` の一対一対応
- sample duration と movie duration の対応
- presentation raster と SSPS camera raster の一致条件
- random access, trim, rebase, transcode, metadata removal の整合性規則
- reader / writer / validator の conformance rule
- malformed / unsupported condition
- resource and security limit

本仕様は video codec, audio codec, color management, audio synchronization, subtitle, caption, timecode, movie-level metadata の payload semantics を再定義しない.

SpatialSnapshot metadata track を除去した file は, 通常の QuickTime Movie として解釈可能でなければならない.

---

## 2. Normative language

本文中の **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT** は必須要件を表す.

SpatialSnapshot QuickTime Binding v1.0 の binding semantics に `SHOULD`, `SHOULD NOT`, `MAY` は使用しない.

QuickTime File Format が複数の合法表現を許す場合, 本仕様が一意に制限した部分については本仕様の制約が優先する.

---

## 3. Normative dependencies

SpatialSnapshot QuickTime Binding v1.0 は以下を前提とする.

1. `SSPS/1.0` および compatible `SSPS/1.x`
2. Apple QuickTime File Format specification, Timed Metadata Media
3. Apple QuickTime File Format specification, Track References
4. Apple QuickTime File Format specification, Movie / Track / Media / Sample Tables
5. ISO/IEC 14496-12:2026, ISO Base Media File Format, for versioned movie/track/media header field widths shared by the host representation

QuickTime atom 内の multi-byte integer は, QuickTime File Format の規則に従い big-endian で格納する.

SSPS payload 内の byte order は SSPS 自身の規則に従う. QuickTime binding は SSPS payload の byte order を変換しない.

---

## 4. Conformance classes

### 4.1 Binding-conforming writer

writer は本文書の全 writer requirement を満たす finalized QuickTime Movie のみを SpatialSnapshot QuickTime v1 として出力しなければならない.

### 4.2 Binding-conforming reader

reader は Section 31 の discovery algorithm に従って SpatialSnapshot timed metadata track を発見し, binding-level validation に成功した場合にのみ SpatialSnapshot data を公開しなければならない.

### 4.3 Binding-conforming validator

validator は以下を独立に報告しなければならない.

- host QuickTime structural failure
- SpatialSnapshot binding malformed condition
- reconstructed SSPS malformed condition
- unsupported SSPS major version
- unsupported critical SSPS extension
- unsupported bound video representation

一つの failure class を別の class として報告してはならない.

---

## 5. Terminology

### 5.1 Host movie

SpatialSnapshot metadata track を含む QuickTime Movie file 全体を **host movie** と呼ぶ.

### 5.2 Bound video track

SpatialSnapshot metadata track の `cdsc` track reference が指す, SpatialSnapshot の presentation raster を提供する visual track を **bound video track** と呼ぶ.

### 5.3 SpatialSnapshot metadata track

Section 10 から Section 17 の宣言条件を満たし, exactly one bound video track を `cdsc` で記述する timed metadata track を **SpatialSnapshot metadata track** と呼ぶ.

### 5.4 Metadata sample

SpatialSnapshot metadata track の一つの timed media sample を **metadata sample** と呼ぶ.

### 5.5 Packet bundle

metadata sample 内の SpatialSnapshot key value atom の payload を **packet bundle** と呼ぶ.

packet bundle は Section 19 に従って SSPS StreamHeader または完全な SSPS packet sequence を保持する.

### 5.6 Presentation frame

bound video track を composition / presentation timeline 順に解釈した一つの decoded visual sample を **presentation frame** と呼ぶ.

B-frame 等により decode order と presentation order が異なる場合, 本仕様の frame order は presentation order のみを意味する.

---

## 6. Host QuickTime requirements

host movie は valid QuickTime Movie file でなければならない.

host movie は少なくとも以下を含まなければならない.

```text
ftyp
moov
  mvhd
  trak                  // bound video track
  trak                  // SpatialSnapshot metadata track
mdat ...
```

`moov` は exactly one 存在しなければならない.

`moov/mvhd.timescale` は厳密に `1_000_000_000` でなければならない. movie timeline の一単位は厳密に `1 ns` である.

SpatialSnapshot QuickTime v1 host movie は finalized non-fragmented movie でなければならない.

以下の atom は存在してはならない.

```text
moof
mfra
```

movie fragment を用いる file は SpatialSnapshot QuickTime v1 として malformed である.

`mdat` atom は一つ以上存在できる. sample table が指す全 media bytes は host movie 内の valid `mdat` payload に完全に含まれなければならない.

---

## 7. File type and suffix

### 7.1 File suffix

SpatialSnapshot QuickTime v1 standard writer は filename suffix として `.mov` を使用しなければならない.

### 7.2 File type atom

host movie は `ftyp` atom を exactly one 含まなければならない.

`ftyp.major_brand` は ASCII FourCC `qt  ` でなければならない.

`compatible_brands` は `qt  ` を含まなければならない.

SpatialSnapshot QuickTime v1 は独自 QuickTime / ISO BMFF brand を定義しない.

SpatialSnapshot の存在判定に `ftyp` 以外の private brand, filename, UTType のみを使用してはならない.

---

## 8. SpatialSnapshot binding cardinality

一つの host movie は exactly one SpatialSnapshot metadata track を持たなければならない.

その SpatialSnapshot metadata track は exactly one bound video track を記述しなければならない.

以下は malformed binding である.

- SpatialSnapshot metadata track が zero
- SpatialSnapshot metadata track が二つ以上
- SpatialSnapshot metadata track の `cdsc` target が zero
- SpatialSnapshot metadata track の `cdsc` target が二つ以上
- `cdsc` target が visual track でない
- bound video track が host movie 内に存在しない

host movie は SpatialSnapshot 以外の metadata track, audio track, subtitle track, caption track, timecode track, その他 QuickTime-valid track を追加で含むことができる.

それらを SpatialSnapshot metadata track として解釈してはならない.

---

## 9. Bound video track

### 9.1 Handler type

bound video track の media handler type は ASCII FourCC `vide` でなければならない.

### 9.2 Track identity

bound video track は non-zero unique `track_ID` を持たなければならない.

host movie 内で `track_ID` は重複してはならない.

### 9.3 Track enablement

bound video track の `tkhd` enabled flag, in-movie flag, in-preview flag はすべて set されていなければならない.

### 9.4 Layer and alternate group

bound video track の layer は `0` でなければならない.

bound video track の alternate group は `0` でなければならない.

### 9.5 Track matrix

bound video track の track matrix は exact identity matrix でなければならない.

container-level rotation, mirror, scale, translation を track matrix に encode してはならない.

### 9.6 Edit list

bound video track は `edts` / `elst` を含んではならない.

bound video media timeline の time `0` は movie presentation timeline の time `0` と一致しなければならない.

### 9.7 Visual sample description count

bound video track の `stsd` は exactly one visual sample description entry を持たなければならない.

video stream 中に sample description を切り替えてはならない.

### 9.8 Raster dimensions

bound video track の `mdia/mdhd.timescale` は厳密に `1_000_000_000` でなければならない.

bound video track の coded/presentation raster dimensions は stream 全体で一定でなければならない.

width および height はそれぞれ `1..16384` の範囲でなければならない.

visual sample entry の width / height はこれらの raster dimensions と一致しなければならない.

`tkhd.width` / `tkhd.height` は QuickTime 16.16 fixed-point 表現で, それぞれ `width << 16` / `height << 16` と厳密に一致しなければならない.

Section 27 に従い, これらは全 SSPS `CAMERA_SAMPLE.raster_width` / `raster_height` と一致しなければならない.

### 9.9 Pixel aspect ratio

visual sample entry に `pasp` atom が存在しない場合, pixel aspect ratio は `1:1` と解釈する.

`pasp` が存在する場合, `hSpacing == vSpacing` でなければならない.

non-square pixel presentation は SpatialSnapshot QuickTime v1 では malformed binding である.

### 9.10 Clean aperture

`clap` atom は存在してはならない.

SpatialSnapshot QuickTime v1 の presentation raster は visual sample の full decoded raster そのものである.

### 9.11 Track-level presentation modifiers

bound video track は `tapt`, `clip`, `matt` atom を含んではならない.

SpatialSnapshot presentation raster は visual sample entry の full decoded raster と identity track matrix のみによって決定されなければならない.

### 9.12 Video codec

SpatialSnapshot QuickTime v1 は bound video codec を一つに固定しない.

codec sample entry と codec-specific configuration は QuickTime File Format および当該 codec specification に対して valid でなければならない.

reader は SpatialSnapshot metadata を抽出するために video codec bitstream を decode することを要求されない.

---

## 10. SpatialSnapshot metadata track structure

SpatialSnapshot metadata track は通常の QuickTime timed metadata track structure を使用しなければならない.

概念構造は以下である.

```text
trak
  tkhd
  tref
    cdsc
  mdia
    mdhd
    hdlr(type='meta')
    minf
      gmhd
      dinf
      stbl
        stsd
          mebx
            keys
              <local_key_id>
                keyd
                dtyp
        stts
        stsc
        stsz | stz2
        stco | co64
```

SpatialSnapshot metadata track は video track, audio track, text track として宣言してはならない.

metadata `minf` は exactly one `gmhd`, exactly one self-contained `dinf`, exactly one `stbl` を含まなければならない. `gmhd` は exactly one `gmin` child を含まなければならない. metadata `minf` に `vmhd` または `smhd` を含めてはならない.

`gmin` は以下の値を持たなければならない.

| Field           | Required value |
| --------------- | -------------: |
| `version`       |            `0` |
| `flags`         |            `0` |
| `graphics_mode` |   `0` (`copy`) |
| `opcolor.red`   |            `0` |
| `opcolor.green` |            `0` |
| `opcolor.blue`  |            `0` |
| `balance`       |            `0` |
| `reserved`      |            `0` |

これらの値は metadata payload の semantic content を変更しない neutral base-media header として固定する.

---

## 11. SpatialSnapshot metadata track header

### 11.1 Track ID

metadata track は non-zero unique `track_ID` を持たなければならない.

metadata track `track_ID` は bound video track `track_ID` と異ならなければならない.

### 11.2 Track flags

metadata track の `tkhd` enabled flag は set されていなければならない.

metadata track の in-movie flag は set されていなければならない.

metadata track の in-preview flag は set されていなければならない.

### 11.3 Width and height

metadata track の `tkhd.width` および `tkhd.height` は両方 `0` でなければならない.

### 11.4 Volume

metadata track の `tkhd.volume` は `0` でなければならない.

### 11.5 Matrix

metadata track の matrix は exact identity matrix でなければならない.

### 11.6 Layer and alternate group

metadata track の layer は `0` でなければならない.

metadata track の alternate group は `0` でなければならない.

### 11.7 Edit list

metadata track は `edts` / `elst` を含んではならない.

metadata media timeline の time `0` は movie presentation timeline の time `0` と一致しなければならない.

---

## 12. `cdsc` track relationship

metadata track の `trak` は `tref` を含まなければならない.

`tref` は exactly one `cdsc` track reference type atom を含まなければならない.

その `cdsc` は exactly one track ID を含み, その値は bound video track の `track_ID` でなければならない.

reference direction は厳密に以下である.

```text
SpatialSnapshot metadata track --cdsc--> bound video track
```

逆方向の `cdsc` は SpatialSnapshot binding を表さない.

SpatialSnapshot metadata track の `cdsc` に audio, subtitle, caption, timecode, 別 metadata track を追加してはならない.

---

## 13. Metadata media header

### 13.1 Handler type

metadata track の `mdia/hdlr.handler_type` は ASCII FourCC `meta` でなければならない.

### 13.2 Media timescale

metadata track の `mdhd.timescale` は厳密に次の値でなければならない.

```text
1_000_000_000
```

したがって metadata media time の一単位は厳密に `1 ns` である.

SSPS `timestamp_ns` は scale conversion を行わず, そのまま metadata media presentation time と一致しなければならない.

### 13.3 Media duration

metadata track の `mdhd.duration` は reconstructed SSPS `STREAM_END.timestamp_ns` と数値的に一致しなければならない.

### 13.4 Language

metadata track の media language field は `0` として書かなければならない.

SpatialSnapshot packet bundle は language-dependent metadata ではない.

---

## 14. Metadata sample description

metadata track の `stsd` は exactly one sample description entry を含まなければならない.

その data format / sample entry type は ASCII FourCC `mebx` でなければならない.

`mebx` sample description は exactly one metadata key table `keys` atom を含まなければならない.

`btrt` は含んではならない.

unknown extension atom が `mebx` sample description 内に存在する場合, QuickTime File Format の generic atom skipping rule に従って無視しなければならない. ただし Section 15 の `keys` semantics を変更する extension は SpatialSnapshot QuickTime v1 として unsupported binding である.

---

## 15. SpatialSnapshot metadata key

### 15.1 Key table cardinality

`keys` table は exactly one active metadata key atom を含まなければならない.

unused key atom (`local_key_id == 0`) を含んではならない.

### 15.2 `local_key_id`

active metadata key atom の atom type が `local_key_id` である.

`local_key_id` は以下を満たさなければならない.

```text
local_key_id != 0x00000000
local_key_id != 0xFFFFFFFF
```

`local_key_id` の具体的な数値は file-local identifier であり, SpatialSnapshot binding identifier ではない.

reader は特定の `local_key_id` 数値を仮定してはならず, `keys` table の `keyd` 内容から discovery しなければならない.

### 15.3 Key declaration

metadata key atom は exactly one `keyd` atom を含まなければならない.

`keyd` は以下を持たなければならない.

| Field           | Required value                                       |
| --------------- | ---------------------------------------------------- |
| `key_namespace` | ASCII FourCC `mdta`                                  |
| `key_value`     | UTF-8 bytes `org.spatialsnapshot.ssps.packet-bundle` |

`key_value` は terminating NUL を持たない.

comparison は byte-for-byte case-sensitive comparison とする.

### 15.4 Datatype declaration

metadata key atom は exactly one `dtyp` atom を含まなければならない.

`dtyp` は以下を持たなければならない.

| Field                | Required value                                     |
| -------------------- | -------------------------------------------------- |
| `datatype_namespace` | `1`                                                |
| `datatype_array`     | UTF-8 bytes `com.apple.metadata.datatype.raw-data` |

`datatype_array` は terminating NUL を持たない.

comparison は byte-for-byte case-sensitive comparison とする.

SpatialSnapshot packet bundle value は raw bytes として解釈する.

### 15.5 Locale

metadata key atom は `loca` atom を含んではならない.

SpatialSnapshot packet bundle に locale semantics を付与してはならない.

### 15.6 Additional declaration atoms

SpatialSnapshot QuickTime v1 writer は `keyd` と `dtyp` 以外の child atom を metadata key atom に出力してはならない.

reader が unknown child atom を発見した場合, atom size に従って skip しなければならない. unknown child atom が key identity または value datatype を置換・上書きする semantics を持つ場合, その binding は unsupported である.

---

## 16. Timed metadata sample data format

各 metadata sample は exactly one metadata value atom から構成されなければならない.

metadata value atom の layout は以下である.

```text
u32 atom_size_be
u32 atom_type_be = local_key_id
u8  packet_bundle[atom_size - 8]
```

`atom_size` は metadata value atom 全体の byte count であり, 8-byte atom header を含む.

`atom_size` は `8..4_294_967_295` の範囲でなければならない.

したがって一つの packet bundle の format-level maximum は `4_294_967_287` bytes である.

`atom_size == 0` および `atom_size == 1` の extended-size representation は metadata value atom に使用してはならない.

metadata sample byte length は `atom_size` と厳密に一致しなければならない.

metadata sample 内に second value atom, padding atom, free atom, unknown atom を追加してはならない.

`atom_type` は Section 15 で discovery された `local_key_id` と一致しなければならない.

---

## 17. Metadata sample table constraints

metadata track は通常の non-fragmented sample table を使用しなければならない.

`stbl` は少なくとも以下を含まなければならない.

```text
stsd
stts
stsc
stsz | stz2
stco | co64
```

### 17.1 Composition offsets

metadata track は `ctts` を含んではならない.

metadata decode time と presentation time は同一でなければならない.

### 17.2 Sync samples

metadata track は `stss` を含んではならない.

`stss` が存在しない QuickTime semantics に従い, 全 metadata sample を sync sample として扱う.

### 17.3 Sample dependency tables

metadata track は `sdtp`, `sgpd`, `sbgp` によって sample dependency, roll distance, dependency group を定義してはならない.

各 packet bundle は他の metadata sample の byte representation に依存せず独立に取得可能でなければならない.

### 17.4 Data references

metadata track の media data reference は host movie 内の self-contained media data を指さなければならない.

external URL / alias / reference movie data source を使用してはならない.

---

## 18. SSPS logical stream reconstruction

SpatialSnapshot metadata track から logical SSPS stream を復元するとき, reader は metadata samples を metadata media presentation time の昇順に処理しなければならない.

各 sample の packet bundle bytes を, その順序で単純連結する.

metadata sample 数を `N`, sample `i` の packet bundle bytes を `B_i` とすると, reconstructed SSPS byte stream `S` は厳密に

```text
S = B_0 || B_1 || ... || B_(N-1)
```

である.

`||` は byte concatenation を表す.

binding layer は packet bundle の間に separator, padding, length prefix, checksum, container header を挿入しない.

この連結結果は SSPS `StreamHeader` の byte 0 から `STREAM_END` packet の末尾までを byte-for-byte 再構成しなければならない.

---

## 19. Packet bundle partitioning

### 19.1 First metadata sample

first metadata sample の presentation time は `0` でなければならない.

first packet bundle は以下をこの順序で含まなければならない.

1. SSPS `StreamHeader` 全体
2. `timestamp_ns == 0` を持つ全 SSPS packet

SSPS canonical ordering により, この packet set は `SCENE_INFO`, initial checkpoint, first `CAMERA_SAMPLE`, 存在する場合の `DEPTH_SAMPLE` を含む.

`timestamp_ns > 0` の packet を first packet bundle に含めてはならない.

### 19.2 Intermediate metadata sample

metadata sample `i > 0` の presentation time を `t_i` とする.

その packet bundle は, SSPS stream 内で `timestamp_ns == t_i` を持つ全 packet を, SSPS stream order のまま exactly once 含まなければならない.

`StreamHeader` を intermediate bundle に含めてはならない.

別 timestamp の packet を intermediate bundle に含めてはならない. ただし final sample の `STREAM_END` について Section 19.3 の一つの例外を適用する.

### 19.3 Final metadata sample

last metadata sample の presentation time を `t_last`, SSPS logical duration を `D = STREAM_END.timestamp_ns` とする.

final packet bundle は以下をこの順序で含まなければならない.

1. `timestamp_ns == t_last` を持つ全 non-`STREAM_END` SSPS packet
2. exact one final `STREAM_END` packet

`STREAM_END.timestamp_ns` は `D` でなければならない.

`STREAM_END` は SSPS stream の最後の packet でなければならない.

`STREAM_END` 専用の追加 metadata sample を time `D` に作成してはならない.

### 19.4 Bundle completeness

一つの SSPS packet の `PacketHeader` または stored payload を複数 metadata sample に分割してはならない.

一つの packet は必ず一つの packet bundle に完全に含まれなければならない.

### 19.5 Exact partition

SSPS StreamHeader および全 SSPS packet byte は exactly once metadata track に格納されなければならない.

重複, 欠落, container-local rewrite は禁止する.

---

## 20. Metadata sample timestamps and durations

metadata samples の presentation times を

```text
t_0, t_1, ..., t_(N-1)
```

とする.

以下を全て満たさなければならない.

```text
t_0 = 0
t_i < t_(i+1)   for every i in [0, N-2]
t_(N-1) < D
```

各 `t_i` は reconstructed SSPS 内の exactly one `CAMERA_SAMPLE.timestamp_ns` と一致しなければならない.

各 `CAMERA_SAMPLE.timestamp_ns` には exactly one metadata sample が存在しなければならない.

metadata sample duration は以下でなければならない.

```text
duration_i     = t_(i+1) - t_i     for i < N-1
duration_last  = D - t_(N-1)
```

全 metadata sample duration は positive integer nanoseconds であり, さらに QuickTime `stts` の 32-bit sample-duration field に収まらなければならない.

```text
1 <= duration_i <= 4_294_967_295
```

したがって隣接する SpatialSnapshot camera timestamps の間隔, および `D - t_last` は `4_294_967_295 ns` を超えてはならない.

metadata track の `stts` が展開する sample decode duration sequence は上式と厳密に一致しなければならない.

---

## 21. SSPS stream requirements

reconstructed SSPS stream は以下を満たさなければならない.

- valid `SSPS/1.x`
- `stream_kind == VIDEO`
- first `CAMERA_SAMPLE.timestamp_ns == 0`
- first camera pose is identity
- `STREAM_END.timestamp_ns > 0`
- SSPS canonical packet ordering を満たす
- SSPS media-binding invariants を満たす

`stream_kind == STILL` の SSPS は QuickTime Binding v1 には格納してはならない.

---

## 22. Video presentation timeline

### 22.1 Presentation time

bound video frame と SSPS の対応には QuickTime video sample の **presentation timestamp** を使用しなければならない.

decode timestamp を SpatialSnapshot timestamp として使用してはならない.

### 22.2 Video composition offsets

bound video track は B-frame その他の frame reordering を表現する場合にのみ `ctts` を含む. frame reordering がない場合, `ctts` は存在してはならない.

`ctts` が存在する場合, reader は各 video sample について QuickTime File Format の式

```text
composition_time = decode_time + composition_offset
```

を適用し, presentation order を composition time の昇順として再構成しなければならない. composition offset の正負を保持しなければならない.

`ctts` table は bound video sample 全体を exactly once cover しなければならず, table expansion の arithmetic overflow は malformed binding である.

`cslg` が存在する場合, その値は `stts` / `ctts` から計算される composition timeline と矛盾してはならない. `cslg` は Section 23 の SpatialSnapshot timestamp を上書きしない.

reader は sample decode order を SpatialSnapshot presentation order として使用してはならない.

### 22.3 Strict frame PTS ordering

presentation order に並べた video frame timestamps は strictly increasing でなければならない.

二つの presentation frame が同じ presentation timestamp を持ってはならない.

### 22.4 Zero origin

first presentation frame の presentation timestamp は movie time `0` でなければならない.

---

## 23. Video timestamp representation

bound video track の `mdhd.timescale` は Section 9.8 により `1_000_000_000` である.

したがって bound video frame の presentation timestamp value は nanoseconds を厳密に表し, scale conversion または rounding を行ってはならない.

video frame `i` の presentation timestamp value を `v_i` とすると, 対応する値は厳密に

```text
video_frame_pts_ns[i]       = v_i
metadata_sample_time_ns[i]  = v_i
CAMERA_SAMPLE.timestamp_ns  = v_i
```

でなければならない.

composition offset を使用する場合も, 最終 presentation timestamp は 1 GHz media timescale 上の整数値として求めなければならない.

negative presentation timestamp は malformed binding である.

---

## 24. One-to-one frame/sample/camera correspondence

bound video track の presentation frame count, SpatialSnapshot metadata sample count, SSPS `CAMERA_SAMPLE` count は全て等しくなければならない.

frame index を presentation order で `i` とすると, 以下の三値は厳密に同じ時刻を表さなければならない.

```text
video_frame_pts_ns[i]
metadata_sample_time_ns[i]
ssps_camera_timestamp_ns[i]
```

したがって

```text
video_frame_count == metadata_sample_count == camera_sample_count
```

でなければならない.

video frame のない metadata sample, metadata sample のない video frame, camera sample のない metadata sample は malformed binding である.

presentation order の frame `i` の presentation duration は metadata sample `i` の Section 20 duration と厳密に一致しなければならない. したがって video と SpatialSnapshot metadata は timestamp だけでなく各 presentation interval `[t_i, t_i + duration_i)` も一対一で一致する.

---

## 25. Movie and media duration

SSPS logical duration を `D` nanoseconds とする.

`mvhd.timescale == 1_000_000_000` であるため `mvhd.duration` は厳密に `D` でなければならない.

metadata track の `mdhd.duration` および `tkhd.duration` は厳密に `D` でなければならない.

bound video track の presentation duration および `tkhd.duration` も厳密に `D` でなければならない.

追加 track の presentation は movie duration `D` を超えてはならない.

last presented video frame は `D` より前に開始し, その frame duration は presentation end `D` に達しなければならない.

`mvhd`, bound video `tkhd` / `mdhd`, metadata `tkhd` / `mdhd` は host specification が定義する version `0` または version `1` の field-width semantics に従わなければならない. version `0` の duration field を使用する atom では `D <= 0xFFFFFFFF` でなければならない. `D > 0xFFFFFFFF` の場合, その duration を保持する当該 header atom は 64-bit duration field を持つ version `1` でなければならない.

header version の選択によって timescale または presentation timestamp の意味論を変更してはならない.

---

## 26. Presentation raster identity

bound video の presentation raster と SSPS camera raster は一つの pixel coordinate system を共有しなければならない.

各 `CAMERA_SAMPLE` について以下が成立しなければならない.

```text
CAMERA_SAMPLE.raster_width  == bound_video_width
CAMERA_SAMPLE.raster_height == bound_video_height
```

pixel center convention は SSPS specification の定義に従う.

QuickTime container layer で以下を適用してはならない.

- rotation
- mirroring
- crop
- clean-aperture remapping
- non-unity pixel aspect ratio
- track-matrix scaling
- track-matrix translation
- `tapt` aperture remapping
- track clipping (`clip`)
- track matte (`matt`)

video decoder の full output raster pixel `(x, y)` は, そのまま SSPS presentation raster pixel `(x, y)` でなければならない.

---

## 27. Media decode independence

SpatialSnapshot metadata track を parse, validate, reconstruct するために bound video bitstream を decode することを要求してはならない.

reader は QuickTime sample table から以下を検証できなければならない.

- video presentation timestamp sequence
- video presentation duration
- visual sample description dimensions
- track matrix
- clean aperture absence
- pixel aspect ratio

codec-specific coded frame crop が visual sample description の declared raster を変える場合, presentation raster は QuickTime/codec specification に基づく final decoded raster と一致しなければならない.

SSPS semantic scene の公開には video pixel decode は必要ない.

---

## 28. Random access semantics

### 28.1 Metadata sample access

metadata samples は通常の QuickTime sample table により random access 可能でなければならない.

reader は target video frame presentation timestamp と同じ metadata sample index を取得できなければならない.

### 28.2 Scene-state seek

時刻 `t` の persistent scene geometry は SSPS checkpoint semantics に従って復元する.

container binding は SSPS checkpoint を別の QuickTime index structure に複製しない.

standard implementation は metadata sample payload 内の packet headers を index し, 各 completed checkpoint の timestamp と metadata sample index の対応を cache しなければならない.

seek 時は `t` 以下で最大 timestamp を持つ completed checkpoint から SSPS geometry updates を適用しなければならない.

### 28.3 Packet bundle locality

checkpoint を構成する `CHECKPOINT_BEGIN`, checkpoint body, `CHECKPOINT_END` は SSPS canonical rule により同一 timestamp を持つため, 同一 metadata sample packet bundle 内に完全に含まれなければならない.

checkpoint を metadata sample boundary で分割してはならない.

---

## 29. Container byte preservation

QuickTime binding は SSPS StreamHeader / PacketHeader / packet payload byte を一切変換してはならない.

以下は禁止する.

- Base64 encoding
- JSON wrapping
- CBOR wrapping
- property-list wrapping
- outer compression
- endian conversion
- packet reserialization
- CRC recalculation without SSPS semantic rewrite

metadata value atom の 8-byte QuickTime atom header のみが SSPS logical stream 外側の binding framing である.

---

## 30. SpatialSnapshot presence detection

SpatialSnapshot QuickTime v1 presence は以下の全条件により判定する.

1. media handler type が `meta`
2. `stsd` entry が exactly one `mebx`
3. `mebx/keys` 内に Section 15.3 の exact `mdta` key が存在する
4. その key の datatype が Section 15.4 の exact raw-data datatype である
5. metadata track が `cdsc` により exactly one `vide` track を指す

filename, track name, handler name, `local_key_id` numeric value, movie-level metadata, private brand を presence detection に使用してはならない.

---

## 31. Reader discovery algorithm

binding-conforming reader は以下をこの順序で実行しなければならない.

1. QuickTime atom tree の structural validity を検証する.
2. top-level `ftyp` が exactly one であり, `major_brand == 'qt  '`, `compatible_brands` が `qt  ` を含むことを確認する. exactly one `moov` を確認し, `mvhd.timescale == 1_000_000_000` を検証する.
3. `moof` / `mfra` が存在しないことを確認する.
4. `moov` 内の全 `trak` を列挙する.
5. handler type `meta` の track を対象集合として列挙する.
6. 各対象 track の `stsd` を読む.
7. exactly one `mebx` sample description を持つ track のみを継続する.
8. `mebx/keys` を parse する.
9. `keyd(namespace='mdta', value='org.spatialsnapshot.ssps.packet-bundle')` を exact match で探索する.
10. matching key の `local_key_id` が reserved value でないことを確認する.
11. matching key の `dtyp` が namespace `1`, value `com.apple.metadata.datatype.raw-data` であることを確認する.
12. SpatialSnapshot key を持つ metadata track が exactly one であることを確認する.
13. その track の `tref/cdsc` target が exactly one であることを確認する.
14. target track を解決し, handler type `vide` であることを確認する.
15. Section 9, 11, 13, 17 の track constraints を検証する.
16. metadata sample table を展開して presentation times と durations を得る.
17. 各 metadata sample が exactly one value atom であることを検証する.
18. value atom type が discovered `local_key_id` と一致することを確認する.
19. packet bundles を Section 18 により連結する.
20. reconstructed bytes が SSPS StreamHeader magic から始まることを確認する.
21. full SSPS parser / validator で reconstructed stream を検証する.
22. packet partition が Section 19 を満たすことを検証する.
23. metadata sample timestamp と `CAMERA_SAMPLE.timestamp_ns` の一対一対応を検証する.
24. bound video presentation frame PTS sequence を求める.
25. Section 23 に従い video PTS が nanosecond 値として直接表現されていることを確認する.
26. Section 24 の frame/sample/camera one-to-one relation を検証する.
27. Section 25 の duration identity を検証する.
28. Section 26 の raster identity を検証する.
29. 全 binding validation が成功した後にのみ SpatialSnapshot scene を公開する.

---

## 32. Writer construction algorithm

binding-conforming writer は以下を実行しなければならない.

1. top-level `ftyp` を exactly one 作成し, `major_brand = 'qt  '`, `compatible_brands` に `qt  ` を含める.
2. source RGB frames, camera samples, geometry/depth events を共通 presentation timeline に同期する.
3. valid SSPS `VIDEO` logical stream を生成する.
4. SSPS full validation を実行する.
5. SSPS `CAMERA_SAMPLE` timestamps を strictly increasing sequence `t_i` として取得する. 隣接差 `t_{i+1}-t_i` および final interval `D-t_last` のいずれかが `1..4_294_967_295 ns` の範囲外なら, file を生成せず `BINDING_UNREPRESENTABLE` とする.
6. movie timescale と bound video media timescale をともに `1_000_000_000` に設定する.
7. bound video presentation frames を同じ `t_i` nanoseconds に等しい整数 PTS で encode / mux する.
8. bound video raster を全 `CAMERA_SAMPLE` raster と一致させる.
9. SpatialSnapshot metadata track を `mdhd.timescale = 1_000_000_000` で作成する.
10. Section 10 の `gmhd/gmin`, self-contained `dinf`, `stbl` を作成する.
11. `meta` handler と `mebx` sample description を作成する.
12. Section 15 の exactly one metadata key declaration を作成する.
13. metadata track から bound video track への exactly one `cdsc` reference を作成する.
14. SSPS byte stream を Section 19 に従い packet bundles に partition する.
15. 各 packet bundle が Section 16 の format-level maximum `4_294_967_287` bytes 以下であることを検証する. 超過する stream は v1 QuickTime binding として出力せず `BINDING_UNREPRESENTABLE` とする.
16. 各 packet bundle を exactly one metadata value atom に格納する.
17. metadata sample presentation time を対応する `CAMERA_SAMPLE.timestamp_ns` に設定する.
18. metadata sample durations を Section 20 に従って設定する.
19. `mvhd.duration`, bound video `tkhd/mdhd.duration`, metadata `tkhd/mdhd.duration` を SSPS duration `D` に一致させる.
20. metadata sample table と media offsets を finalize する.
21. host movie を non-fragmented finalized QuickTime file として finalize する.
22. finalized bytes を再度 reader path で parse する.
23. binding, SSPS, frame timestamp, duration, raster の validation をすべて成功させる.
24. validation success 後にのみ output を complete file として公開する.

---

## 33. Canonical trim and rebase

SpatialSnapshot-aware temporal trim は SSPS metadata track の sample 範囲を単純切断してはならない.

trim interval は SSPS `Canonical trim and rebase` の有効区間 `[t_start, t_end)` でなければならない.

writer は以下を実行しなければならない.

1. original SSPS を full reconstruct / validate する.
2. SSPS specification に従って `[t_start, t_end)` の canonical trim and rebase を実行する.
3. output SSPS の new time zero で camera pose を identity にする.
4. output SSPS に新しい random `stream_id` を生成する.
5. bound video を同じ interval へ trim する.
6. first retained video presentation frame が new time `0` になるよう media timestamps 自体を rebase する.
7. `elst` による time shift を使用してはならない.
8. output video frame PTS sequence を output SSPS `CAMERA_SAMPLE.timestamp_ns` と一致させる.
9. SpatialSnapshot metadata track を Section 19 の packet partition rule から完全に再生成する.
10. metadata sample times / durations を new SSPS duration から再生成する.
11. output movie duration を new SSPS duration と整合させる.
12. full binding validation を実行する.

geometry checkpoint の再構成は SSPS canonical trim rule に従う. container binding は旧 checkpoint bytes をそのまま先頭 sample へ移動してはならない.

---

## 34. Temporal editing rules

### 34.1 Frame dropping

bound video frame を削除する編集では, 対応する SSPS `CAMERA_SAMPLE` と同 timestamp の geometry/depth packet semantics を含めて SSPS stream を再生成しなければならない.

旧 metadata track を保持してはならない.

### 34.2 Frame insertion or interpolation

新しい presentation frame を挿入する編集では, その frame timestamp に対応する valid `CAMERA_SAMPLE` を生成できない限り SpatialSnapshot metadata を保持してはならない.

生成できない場合, SpatialSnapshot metadata track 全体を除去しなければならない.

### 34.3 Speed change

playback speed, time stretch, reverse, variable retiming を行う編集では, video PTS と全 SSPS packet timestamps を semantic に再生成し, SSPS invariants を満たす場合にのみ SpatialSnapshot を保持できる.

単なる `elst` による retiming で旧 SSPS を保持してはならない.

### 34.4 Reverse playback

時間反転した SpatialSnapshot output は v1 canonical SSPS geometry update semantics をそのまま逆順にして生成してはならない.

reverse output を SpatialSnapshot として保持する場合, 全時刻の scene state を新しい forward timeline として再-synthesize し, valid SSPS stream を新規生成しなければならない.

---

## 35. Spatial editing rules

以下のいずれかが bound video raster geometry を変える場合, SpatialSnapshot-aware editor は SSPS camera model, geometry/depth mapping を整合するよう再生成しなければならない.

- crop
- resize
- rotate
- mirror
- aspect-ratio conversion
- non-unity pixel-aspect conversion
- lens-distortion correction that changes presentation pixel geometry
- stabilization that warps individual frames

SSPS を再生成できない場合, SpatialSnapshot metadata track を完全に除去しなければならない.

old SSPS + spatially transformed video の組み合わせを出力してはならない.

---

## 36. Codec transcode preservation

video codec のみを transcode し, 以下のすべてを byte-independent semantic に保持する場合, existing SSPS logical stream を変更せず再利用できる.

- presentation frame count
- presentation PTS sequence under Section 23 nanosecond representation
- presentation duration
- raster width
- raster height
- pixel coordinate orientation
- crop
- pixel aspect ratio
- per-frame geometric mapping

transcode 後の QuickTime file では SpatialSnapshot metadata track と sample tables を host movie に対して再-mux し, full binding validation を行わなければならない.

video compressed bytes の一致は要求しない.

---

## 37. Metadata removal

SpatialSnapshot metadata を除去する操作は atomic に以下を行わなければならない.

1. SpatialSnapshot metadata `trak` 全体を除去する.
2. その track を参照する stale track relationship があれば除去する.
3. SpatialSnapshot metadata samples が占有していた media bytes は output sample tables から一切参照しない.
4. output QuickTime Movie を通常の valid movie として finalize する.

bound video track 自体は SpatialSnapshot metadata removal のために削除してはならない.

packet bundles の一部だけを残して SpatialSnapshot として扱ってはならない.

---

## 38. Copy and remux rules

SpatialSnapshot-aware remuxer が bound video と SpatialSnapshot metadata を別 container へ copy する場合, 以下を保持しなければならない.

- video presentation timestamp sequence
- metadata sample timestamp sequence
- metadata sample durations
- `cdsc` relationship semantics
- packet bundle bytes
- raster identity
- logical duration

QuickTime 内で track ID を変更する場合, metadata track の `cdsc` target ID も同一 transaction 内で更新しなければならない.

`local_key_id` を変更する場合, `keys` table の metadata key atom type と全 metadata sample value atom type を同じ値へ更新しなければならない.

SSPS packet bytes 自体を `local_key_id` 変更のために変更してはならない.

---

## 39. Integrity model

QuickTime binding は SSPS 以外の新しい checksum algorithm を定義しない.

packet bundle 内の SSPS StreamHeader / PacketHeader / raw payload CRC は SSPS specification の CRC32C rules に従う.

QuickTime atom structure が valid でも SSPS CRC failure が存在する場合, その file は SpatialSnapshot scene として invalid である.

SSPS CRC failure を QuickTime atom parse failure として報告してはならない.

container rewrite が SSPS bytes を変更した場合, writer は SSPS specification に従って正規に再serializeして CRC を再生成するか, SpatialSnapshot metadata を除去しなければならない.

---

## 40. Atomic write requirements

SpatialSnapshot-aware writer / editor は output update を atomic logical transaction として扱わなければならない.

persistent output として以下を残してはならない.

- new video + old incompatible metadata track
- old video + new incompatible metadata track
- metadata track with dangling `cdsc`
- changed metadata `local_key_id` + stale sample atom types
- rewritten SSPS + stale sample sizes / offsets
- trimmed video + unrebased SSPS
- complete `moov` + partial metadata media payload

write failure 時は pre-edit valid file または no output のいずれかでなければならない.

---

## 41. Resource and security limits

SpatialSnapshot QuickTime v1 standard implementation は untrusted input に対して以下の resource limit を適用しなければならない. これらは format-level syntax maximum とは独立した standard implementation の安全上限である.

| Resource                                                        |                     Maximum |
| --------------------------------------------------------------- | --------------------------: |
| Host file bytes                                                 | `274_877_906_944` (256 GiB) |
| Atom nesting depth                                              |                        `64` |
| Parsed atom count                                               |                 `2_097_152` |
| Track count                                                     |                     `1_024` |
| Sample-description entries per track                            |                       `256` |
| Metadata key atoms                                              |                    `65_536` |
| Metadata samples                                                |                `10_000_000` |
| Single packet bundle bytes processed by standard implementation |     `1_073_741_824` (1 GiB) |
| Reconstructed SSPS bytes                                        | `137_438_953_472` (128 GiB) |
| Video raster width                                              |                    `16_384` |
| Video raster height                                             |                    `16_384` |
| Track references per track                                      |                    `65_536` |
| Sample-table entries after run expansion                        |                `20_000_000` |

この table の resource limit 超過は malformed byte syntax ではなく resource-limit rejection として報告しなければならない. Section 16 の `4_294_967_287` byte format-level packet-bundle maximum を超える representation は malformed である.

reader は integer addition, multiplication, offset calculation, sample-count expansion, atom-size calculation に checked arithmetic を使用しなければならない.

file offset + byte count が file size を超える operation を行ってはならない.

metadata packet bundle を allocation する前に declared sample size と Section 41 resource limit を検証しなければならない.

SSPS decompression limits は SSPS specification の制限を追加で適用する.

---

## 42. Host QuickTime malformed conditions

以下は host-level malformed condition である.

1. invalid atom size
2. atom extends outside parent
3. top-level atom extends outside file
4. invalid extended-size encoding
5. invalid `moov` structure
6. missing required sample-table atom
7. sample table references bytes outside valid `mdat`
8. sample size arithmetic overflow
9. sample count mismatch among sample-table structures
10. invalid track ID graph
11. duplicate `track_ID`
12. invalid timescale `0`
13. cyclic or structurally invalid atom hierarchy

host-level malformed condition がある file から SpatialSnapshot scene を公開してはならない.

---

## 43. SpatialSnapshot binding malformed conditions

以下のいずれかが成立した場合, file は SpatialSnapshot QuickTime v1 binding として malformed である.

1. missing SpatialSnapshot metadata track
2. duplicate SpatialSnapshot metadata track
3. SpatialSnapshot metadata handler type が `meta` でない
4. SpatialSnapshot metadata `stsd` が exactly one entry でない
5. sample entry type が `mebx` でない
6. missing `keys`
7. multiple `keys`
8. active SpatialSnapshot key が exactly one でない
9. reserved `local_key_id`
10. missing `keyd`
11. duplicate `keyd`
12. key namespace が `mdta` でない
13. key value mismatch
14. missing `dtyp`
15. duplicate `dtyp`
16. datatype namespace mismatch
17. raw-data datatype string mismatch
18. `loca` present
19. missing `cdsc`
20. duplicate `cdsc`
21. `cdsc` target count not one
22. `cdsc` target missing
23. `cdsc` target handler not `vide`
24. metadata track width or height non-zero
25. metadata track volume non-zero
26. metadata track matrix non-identity
27. metadata track layer non-zero
28. metadata track alternate group non-zero
29. metadata track edit list present
30. movie, bound video, or metadata media timescale not `1_000_000_000`
31. metadata track duration mismatch
32. metadata `ctts` present
33. metadata `stss` present
34. metadata sample has zero or multiple value atoms
35. metadata value atom uses size `0` or extended size
36. metadata value atom type differs from `local_key_id`
37. metadata sample size differs from contained value atom size
38. packet bundle exceeds Section 16 format-level maximum
39. packet bundle splits an SSPS packet
40. packet bundle contains bytes belonging to another timestamp contrary to Section 19
41. first bundle does not begin with exact SSPS StreamHeader
42. StreamHeader occurs more than once
43. `STREAM_END` not in final metadata sample
44. separate metadata sample exists solely at `STREAM_END` time
45. reconstructed byte stream has missing or duplicate SSPS bytes
46. reconstructed SSPS stream kind not VIDEO
47. metadata sample count differs from camera sample count
48. metadata sample timestamps not strictly increasing
49. first metadata sample timestamp not zero
50. metadata sample timestamp differs from matching camera timestamp
51. metadata sample duration differs from Section 20
52. last metadata sample does not end exactly at SSPS duration
53. bound video edit list present
54. bound video track matrix non-identity
55. bound video alternate group non-zero
56. bound video layer non-zero
57. bound video `stsd` count not one
58. bound video `tkhd.width/height` do not equal the full raster in 16.16 fixed-point
59. bound video `clap` present
60. bound video non-square `pasp`
61. video raster differs from SSPS raster
62. video presentation frame count differs from metadata sample count
63. video frame PTS ns differs from corresponding metadata sample timestamp
64. duplicate video presentation PTS
65. first video presentation PTS not zero
66. bound video presentation duration differs from SSPS duration
67. `moof` present
68. `mfra` present
69. host movie uses fragmented presentation
70. metadata media data is externally referenced
71. bytes exist in metadata sample after its single value atom
72. metadata `minf` が required `gmhd` / `dinf` / `stbl` structure を満たさない
73. bound video enabled / in-movie / in-preview flag のいずれかが unset
74. metadata enabled / in-movie / in-preview flag のいずれかが unset
75. bound video `tapt`, `clip`, or `matt` present
76. metadata sample duration が `1..4_294_967_295 ns` の範囲外
77. presented video frame duration differs from corresponding metadata sample duration
78. `mvhd.duration`, bound video `tkhd/mdhd.duration`, or metadata `tkhd/mdhd.duration` differs from `D`
79. header version/field width cannot represent required duration without overflow
80. `ctts` table does not exactly cover all bound video samples or composition arithmetic overflows
81. `cslg` contradicts the `stts` / `ctts` composition timeline
82. top-level `ftyp` が exactly one でない
83. `ftyp.major_brand` が `qt  ` でない
84. `ftyp.compatible_brands` が `qt  ` を含まない
85. metadata `gmhd` が exactly one `gmin` を含まない, または `gmin` field が Section 10 の固定値と一致しない

---

## 44. Unsupported conditions

以下は structurally malformed ではなく unsupported condition である.

1. reconstructed SSPS major version が `1` でない
2. reconstructed SSPS が unknown critical packet / extension を含む
3. future `mebx` extension が SpatialSnapshot key identity または datatype semantics を変更する
4. QuickTime atom / sample-entry version が current implementation の safe parsing capability を超え, その atom を skip すると SpatialSnapshot semantics を決定できない

bound video codec を decoder が decode できないこと自体は SpatialSnapshot metadata extraction の unsupported condition ではない.

その場合, reader は SpatialSnapshot scene geometry, camera, depth, timeline を公開できる. ただし decoded video pixels を必要とする API は video-decoder-unavailable として失敗しなければならない.

---

## 45. Decoder exposure rule

reader は以下のすべてが成功する前に SpatialSnapshot scene を valid scene として public API へ公開してはならない.

1. host QuickTime structural validation
2. binding discovery and validation
3. exact SSPS byte reconstruction
4. SSPS full validation
5. frame/sample/camera timestamp correspondence validation
6. duration validation
7. raster validation

partial parse data は diagnostics にのみ使用できる.

video codec decode success は SpatialSnapshot scene semantic data 公開の前提ではない.

---

## 46. Error classification

standard implementation は少なくとも以下の error domain を区別しなければならない.

```text
QUICKTIME_MALFORMED
BINDING_MALFORMED
BINDING_UNREPRESENTABLE
SSPS_MALFORMED
UNSUPPORTED_BINDING
UNSUPPORTED_SSPS
RESOURCE_LIMIT
VIDEO_DECODER_UNAVAILABLE
IO_ERROR
```

各 domain の意味は以下である.

- `QUICKTIME_MALFORMED`: SpatialSnapshot の有無とは独立した host QuickTime structure が invalid.
- `BINDING_MALFORMED`: host structure は parse できるが SpatialSnapshot QuickTime v1 binding rule に違反.
- `BINDING_UNREPRESENTABLE`: source media / SSPS は valid だが, v1 binding の整数幅または packet-bundle 上限では lossless に表現不能. writer-side failure であり, 既存 file の malformed classification には使用しない.
- `SSPS_MALFORMED`: reconstructed SSPS が SSPS v1 syntax / invariant に違反.
- `UNSUPPORTED_BINDING`: future host/binding construct の semantics を安全に決定できない.
- `UNSUPPORTED_SSPS`: SSPS major version または critical extension が未対応.
- `RESOURCE_LIMIT`: syntax 上は表現可能だが standard implementation の Section 41 hard limit を超える.
- `VIDEO_DECODER_UNAVAILABLE`: spatial metadata は valid だが requested decoded RGB operation に必要な video decoder が利用不能.
- `IO_ERROR`: file bytes の取得・永続化に失敗.

一つの error が後続 validation を安全に継続不能にした場合, validator はその dependency に依存する検査を skipped として記録できる. ただし既に独立して検出可能な error を一つに潰して報告してはならない.

---

## 47. Versioning

SpatialSnapshot QuickTime Binding version は SSPS version と独立する.

`SpatialSnapshot-QuickTime/1.x` は以下を変更してはならない.

- one SpatialSnapshot metadata track cardinality
- metadata handler type `meta`
- timed metadata sample entry `mebx`
- metadata key namespace `mdta`
- metadata key string `org.spatialsnapshot.ssps.packet-bundle`
- raw-data datatype identity
- metadata-track-to-video-track `cdsc` direction
- movie, bound-video, metadata timescales `1_000_000_000`
- one metadata sample per `CAMERA_SAMPLE`
- packet bundle concatenation rule
- first bundle StreamHeader rule
- final bundle `STREAM_END` rule
- presentation-time one-to-one mapping
- presentation raster identity rule
- no edit-list rule
- non-fragmented host movie rule

binding major version の変更は, v1 reader が同じ track/sample graph を正しく解釈できなくなる変更に対してのみ行う.

compatible SSPS v1 minor extension は, その SSPS stream が SSPS v1 forward-compatibility rule を満たす限り QuickTime Binding v1 に格納できる.

---

## 48. Normative external references

1. **Apple QuickTime File Format**, Timed Metadata Media.
2. **Apple QuickTime File Format**, Timed Metadata Sample Description (`mebx`).
3. **Apple QuickTime File Format**, Metadata Key Table / `keyd` / `dtyp`.
4. **Apple QuickTime File Format**, Track Reference Type `cdsc`.
5. **Apple QuickTime File Format**, Movie / Track / Media / Sample Table atoms.
6. **ISO/IEC 14496-12:2026**, ISO Base Media File Format.
7. **SpatialSnapshot Packet Stream (SSPS) Version 1.0**, `Specs/SSPS-v1.md`.
