# Codec interoperability fixtures

このプロジェクトのテスト用に生成した合成メディアです. 

| ファイル | 内容 |
| --- | --- |
| source.heic | 64 × 64, 赤, HEVC still |
| grid.heic | 128 × 64, 赤・青, 64 × 64 の HEVC tile 2 枚による grid primary |
| source.mov | 64 × 48, 青, RGB24 raw video, 10 fps, 3 frames, 0.3 秒, movie / video timescale 1 GHz |
| still.ssps / grid.ssps / video.ssps | 各メディアの raster・timeline に対応する SSPS |

画像は FFmpeg の単色入力と libheif, 動画は FFmpeg, SSPS は conformance encoder で生成しています. 画像・動画は SSPS 組み込み前の入力です. repository root の `sh Scripts/check-binding-interop.sh` で binding・stripping 後の画素と時刻を照合します. 
