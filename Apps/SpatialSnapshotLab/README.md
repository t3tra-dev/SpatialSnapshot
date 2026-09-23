# SpatialSnapshot Lab

SSPS 付きの画像・動画を扱うデモアプリです. iOS 17 以降で収録・検査, macOS 14 以降で Editor・検査を利用できます. macOS では Recorder は無効です.

## 起動

1. [SpatialSnapshotLab.xcodeproj](SpatialSnapshotLab.xcodeproj) を Xcode 26 以降で開きます.
2. scheme に **SpatialSnapshotLab**, 実行先に **My Mac** または iPhone / iPad を選びます.
3. 実機への署名が必要な場合は, 以下のローカル設定を作成してから Run します.

repository root でテンプレートをコピーし, `Signing.local.xcconfig` の `YOUR_TEAM_ID` を自分の Apple Developer Team ID に置き換えてください.

```sh
cp -n Apps/SpatialSnapshotLab/Configuration/Signing.local.xcconfig.example \
  Apps/SpatialSnapshotLab/Configuration/Signing.local.xcconfig
```

ローカル設定は Git の対象外で, Debug / Release の両方に適用されます. Xcode の Team 欄を変更すると共有 project に書き戻されるため, Team ID はこのファイルで編集します. CI と署名なしビルドではローカル設定は不要です.

root の Swift Package と GLTFKit2 の package 定義をローカル参照します. GLTFKit2 0.5.15 のバイナリは SwiftPM が上流から取得して SHA-256 を検証するため, 初回ビルドにはネット接続が必要です. [配布方針](Vendor/GLTFKit2/README.md)を参照してください. repository root から署名なしでビルドを確認できます.

```sh
sh Scripts/build-lab.sh macOS
sh Scripts/build-lab.sh simulator
sh Scripts/build-lab.sh iOS
```

## macOS Editor

1. 内蔵 **Room**, ライブラリ, または **ファイルを開く**から収録を開きます.
2. 床・壁をクリックして追加位置を選び, **追加 → キューブ / glTF・GLB**で配置します.
3. オブジェクトを選択し, ギズモまたは右側の **位置・回転・スケール**を編集します. 位置は m, 回転は度です.
4. Cube の **カラー**では 12 色のパレットと **カスタム**から色を選べます. 一覧から複製・削除・名前変更・表示切替ができます.

視点は収録カメラに固定されます. 表示は **画像 / 深度 / ワイヤーフレーム**を切り替えられ, 画像全体を縦横比を保って表示枠に収めます. 動画はスライダーで対象フレームを選びます.

オブジェクトは追加位置の面に接触させ, 向きは空間軸に合わせます. 中央ハンドルは床・壁に沿った移動, 軸ハンドルは自由移動です. **追加位置に接するよう配置**で再接触できます.

| 操作                   | ショートカット             |
| ---------------------- | -------------------------- |
| 移動 / 回転 / スケール | G / R / S                  |
| スナップ               | Shift                      |
| 複製 / 削除            | Shift+D または ⌘D / Delete |
| Undo / Redo            | ⌘Z / ⇧⌘Z                   |
| ドラッグ取消 / 保存    | Esc / ⌘S                   |

ショートカットはビューポートをクリックして使います. 編集内容と Cube の色は収録ごとに自動保存します. Undo/Redo はセッション内のみです. 編集オブジェクトは元の SSPS・画像・動画には埋め込みません.

glTF 2.0 の静的な三角形メッシュ, 基本 PBR 材質, PNG / JPEG テクスチャに対応します. 関連 bin / 画像がある場合は, モデル 1 個と関連ファイルを含むフォルダを選びます. 合計 512 MiB が上限です. スキン・モーフ・アニメーション, Draco / KTX2 が必須のモデルは対象外です.

## Inspector と共有

**ファイルを開く**で SSPS 付き HEIC / HEIF / MOV, または SSPS 単体を読み込みます. 内蔵 Room は合成サンプルです. **映像 / 深度 / 3D**で検査し, 映像上の床・壁を選ぶと面の情報を表示します.

縦向き表示は保存された重力から決め, 回転ボタンで調整できます. 保存画像はセンサー方向のままなので, 一般のメディアプレイヤーでは横向きに見える場合があります. 映像 codec が使えなくても, 検証済みの空間データは検査できます.

iOS の **共有**は標準共有シートを開き, SSPS を含む画像・動画を 1 ファイルで共有します. SSPS 単体は共有対象外です. macOS の **書き出す**は元ファイル・SSPS 単体・空間データを除いたメディアを保存できます. MOV のメタデータ除去では未参照の payload が残る場合があります.

**範囲を切り出す**は選択した開始〜終了フレームを含む新しいファイルを生成します. ライブラリの収録は行を左へスワイプして削除できます. import の上限は 256 MiB です.

## iOS Recorder

**Recorder → カメラを開始**でカメラ権限を要求します. 追跡が安定してから写真・動画を収録し, 保存後に Inspector で開きます. ARKit 対応実機が必要で, Simulator では収録できません.

LiDAR 対応機ではメッシュ・分類・深度を, 非対応機では RGB とカメラ姿勢を保存します. カメラは全画面表示で, **保存メッシュ**を重ねて確認できます. 操作 UI とデバッグ表示は保存画像に焼き込みません.

動画は音声なし, 最大 30 秒・15 fps, メッシュ更新間隔は最短 0.5 秒です. 入力面数の固定上限はありませんが, メモリと処理量の上限は適用されます. バックグラウンド移行・セッション中断・カメラ停止時は未保存の収録を破棄します.

## 保存先とライセンス

収録は `Documents/SpatialSnapshotLab`, 編集内容は `EditorProjects/<stream-id>/scene.json`, 取り込んだモデルは同プロジェクトの `Assets/` に保存します. 撮影データの自動ネットワーク送信や Photos への自動保存はありません.

アプリと合成デモは [Apache License 2.0](../../LICENSE) です. 同梱ライブラリは [第三者ライセンス](../../THIRD_PARTY_NOTICES.md), ライブラリの利用条件は [Apple adapters](../../Docs/Apple-Adapters.md) を参照してください.
