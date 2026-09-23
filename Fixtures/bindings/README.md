# Binding fixtures

境界条件・不正入力・未知拡張を含む parser 検証用 corpus です. [manifest.json](manifest.json) の `status` は binding / SSPS 読み取り処理の期待結果で, 画像 codec の妥当性や一般のプレイヤーでの再生を保証しません. 

- `heif-*.heif` は正常系にも不完全な JPEG placeholder を含み, 表示用には使えません. 
- `qt-*.mov` は極小の RGB24 raster を使い, プレイヤーによっては復号できません. 
- [interop](interop/README.md) は実 codec を使った合成画像・動画です. 
- 表示用の SSPS 付きサンプルは [Lab の Resources](../../Apps/SpatialSnapshotLab/Resources) にあります. 

```sh
python3 Tests/Bindings/test_bindings.py build/ssvalidate
sh Scripts/check-binding-interop.sh
```

repository root から実行します. 相互運用検査は `build/binding-interop/bound.heic`, `grid-bound.heic`, `bound.mov` を生成します. FFmpeg / libheif を使用し, Apple decoder の検査には macOS のメディアサービスへ接続できる環境が必要です. 
