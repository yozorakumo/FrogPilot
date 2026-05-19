# FrogPilot (Mazda2 DJ MT カスタム版)

このリポジトリは、[yozorakumo/FrogPilot](https://github.com/yozorakumo/FrogPilot) による、**マツダ Mazda2 (DJ) 6MT モデル** への完全対応を行ったカスタムブランチです。

## 🌟 主な追加機能と修正点

本ブランチ (`FrogPilot`) では、マニュアルトランスミッション（MT）車両特有の挙動をサポートするために以下の実装を行っています。

### 1. デュアル信号ギアポジション認識
車両のネイティブ CAN 信号の解析により、2つの信号を組み合わせた高精度なギア判定を実現しました。
- **大分類信号**: `0x166` (NEW_MSG_28) `GEAR_POS` — Forward/Reverse/Neutral の大分類
  - `GEAR_POS=4,5`: Forward（前進）
  - `GEAR_POS=6`: Reverse（リバース）
  - その他: Neutral（ニュートラル）
  - **DBC定義修正**: 当初 `23|4@0+` → 実測データに基づき `20|4@0+` に修正
- **ギア段信号**: `0x165` (PEDALS) `GEAR_POS` — 1速〜6速の具体的ギア段
  - 値マッピング: `2=6th`, `3=5th`, `4=4th`, `5=3rd`, `7=2nd`, `13=1st`
- **クラッチ検出**: PEDALS `GEAR_POS`値が確定ギア値 `{2, 3, 4, 5, 7, 13}` 以外の場合にクラッチが踏まれていると判定
- **判定フロー**: NEW_MSG_28で大分類 → PEDALSでギア段（前進時のみ）
- **メリット**: RPM 比率による推定ではなく、CAN信号から直接ギア状態を取得。リバース検出も正確に実行可能

### 2. ステアリング角度センサーの二重化
- **プライマリ**: `STEER2` (0x86) — 通常時のステアリング角度ソースとして使用
- **フォールバック**: `STEER` (0x82) — STEER2が異常値（±360°超過）の場合のみ使用
- **問題背景**: `STEER`(0x82)は右ウインカー/ハザード時に約26%の確率で異常値（1664°等）を出力するため、プライマリから外了
- **異常値ガード**: `abs(steer_angle) > 360` を検出した場合、フォールバック→前回有効値の順に復元

### 3. MT車最適化制御
- **クラッチ連動ディスエンゲージ**: `GEAR_POS`信号に基づき、クラッチペダルが踏まれた状態（ギア未確定）を検出して、安全にオープンパイロットの制御を解除（ディスエンゲージ）します。
- **エンスト防止ロジック**: 縦方向制御において、MT 車の特性に合わせた加減速の調整を行っています。
- **判定方式**: NEW_MSG_28(0x166)で大分類 + PEDALS(0x165)の`GEAR_POS`でギア段を判定。`0x09E`の信号（CLUTCH_ALT, NEUTRAL_SW）は実車検証で常に0であることが確認され、使用されていません。

### 4. pandaセーフティ MT対応
- **safetyParam=2** (`MAZDA_PARAM_MT`): MT車用のセーフティパラメータを追加。AT車用のCRZ_CTRLチェックをスキップし、ボタンベースの制御に切り替え
- **CRZ_BTNS メインボタン制御**: `MODE_X && MODE_Y` の立ち上がりエッジで `controls_allowed` をトグル。キャンセルボタン（`CAN_OFF`）で `controls_allowed` を無効化
- **AT車との互換性**: AT車は従来通りCRZ_CTRLのACC信号で `controls_allowed` を管理

### 5. SET_P/SET_M ボタンイベント
- クルーズコントロールの速度+/-ボタン（`SET_P`, `SET_M`）のイベントを追加
- `SET_P` → `accelCruise`（速度増加）、`SET_M` → `decelCruise`（速度減少）としてボタンイベントを生成
- MT車の longitudinal control で速度調整に使用

### 6. 青信号アラートのCEM依存解消
- 青信号（信号機）検出を Conditional Experimental Mode (CEM) の有効/無効に関わらず常時実行するよう変更
- `stop_sign_and_light()` がCEM状態によらず常に呼び出され、CEMがオフでも青信号アラートが機能

### 7. MT専用 UI 表示
- **ギア表示**: 走行中のギア数値（1-6速）をダッシュボードにリアルタイム表示（`paintMTGear`）
- **クラッチ表示**: クラッチが踏まれている状態を視覚的にフィードバック
- **BrakePBClutchUI トグル**: ブレーキペダル・パーキングブレーキ・クラッチの状態表示をトグルで切り替え可能（`paintBrakePBClutchStatus`）

### 4. 正確な車両識別 (Fingerprinting)
- Mazda2 DJ MT モデル固有の ECU（カメラ、レーダー、EPS、エンジン、ABS）のファームウェアバージョンをデータベースに登録。
- MT 車に存在しない TCM（トランスミッション制御モジュール）を定義から除外することで、迅速かつ正確な車両識別を可能にしました。

## 🚀 インストール方法 / Quick Start (Japanese)

実機（comma 3 / 3X 等）のセットアップ画面で、以下の URL を入力するだけでインストールが可能です。

1.  デバイスを **工場出荷状態 (Factory Reset)** にするか、ソフトウェアをアンインストールします。
2.  再起動後のセットアップ画面で **[Custom Software]** (またはカスタムURL入力) を選択します。
3.  以下の URL を入力します：
    ```
    https://opkr.o-r.kr/fork/yozorakumo/test-mazda2-dj-mt-frog
    ```

### 💡 インストール時の注意点
- 安定した電源供給とネットワーク環境での作業を推奨します。
- もしインストールが失敗する場合は、ネットワーク接続やデバイスの空き容量を再度確認してください。

## 🛠 開発・調査の記録
今回の対応にあたって実施した CAN バス解析の詳細は、プロジェクト内の設計ドキュメントを参照してください。

### Mazda 2 DJ MT (FrogPilot) 修正履歴

#### 1. ISO-TP Flow Control許可の追加 (`safety_mazda.h`)
- **問題**: バイトオフセット修正（`data[1]==0x3E`）後、"Can Error Check connections"エラーが発生
- **原因**: ISO-TP Flow Controlフレーム（`0x30`）がセーフティホワイトリストでブロックされていた
- **修正**: `data[0] >> 4` でフレームタイプを抽出し、Flow Control（type=`0x3`）を許可リストに追加
- **ファイル**: [`panda/board/safety/safety_mazda.h`](panda/board/safety/safety_mazda.h)

#### 2. ステアリング角度センサーの二重化 (`carstate.py`)
- **問題**: 右ウインカー/ハザード時にハンドルマークが右に急激に回転したまま戻らない
- **原因**: `STEER`(0x82)のステアリング角度が右ウインカー時に約26%の確率で異常値（1664°等）を出力
- **修正**: `STEER2`(0x86)をプライマリソースに変更。`abs(steer_angle) > 360` の異常値を検出した場合、`STEER`(0x82)にフォールバック、さらに前回の有効値で保持
- **ファイル**: [`selfdrive/car/mazda/carstate.py`](selfdrive/car/mazda/carstate.py)

#### 3. デュアル信号ギアポジション判定 (`carstate.py`, `mazda_2_dj_mt.dbc`)
- **問題**: MT車なのにAT用のGEAR信号を読んでいた。また単一信号ではリバース検出が不可能だった
- **原因**: `GEAR`（AT用、`48|5@1+`）ではなく`GEAR_POS`（MT用、`55|8@0+`）を使用すべきだった
- **修正**: NEW_MSG_28(0x166) + PEDALS(0x165) のデュアル信号方式に変更
  - **NEW_MSG_28(0x166)**: GEAR_POSで大分類（Forward/Reverse/Neutral）
    - DBC定義修正: `23|4@0+` → `20|4@0+`（実測データに基づくビットオフセット修正）
    - `GEAR_POS=6` でリバース検出
  - **PEDALS(0x165)**: GEAR_POSでギア段（1-6速）判定
- **ファイル**: [`selfdrive/car/mazda/carstate.py`](selfdrive/car/mazda/carstate.py), [`opendbc/mazda_2_dj_mt.dbc`](opendbc/mazda_2_dj_mt.dbc)
- **値マッピング**: `2=6th`, `3=5th`, `4=4th`, `5=3rd`, `7=2nd`, `13=1st`

#### 4. サイドブレーキ・クラッチ・ニュートラルのCAN信号調査
- **サイドブレーキ (PARKING_BRAKE)**: CAN ID `0x09F` (159), MSG_11, byte0 bit4、ON=1/OFF=0
  - DBC定義に `SG_ PARKING_BRAKE : 4|1@0+` を追加
- **クラッチ**: `GEAR_POS`ベースの間接検出を実装
  - `CLUTCH_PRESSED` (0x165 byte5 bit3) → **常に0、クラッチ信号ではない**（DBCから削除済み）
  - `CLUTCH_ALT` (0x09E byte0 bit5) → **常に0、クラッチ信号ではない**（DBCから削除済み）
  - **解決策**: `GEAR_POS`値が確定ギア値 `{2, 3, 4, 5, 7, 13}` 以外の場合にクラッチが踏まれていると判定
- **ニュートラル**: NEW_MSG_28のGEAR_POSがForward(4,5)以外で判定
- **リバース**: NEW_MSG_28のGEAR_POS=6で判定（従来はリバース検出手段がなかった）

#### 5. pandaセーフティ MT対応 (`safety_mazda.h`)
- **safetyParam**: MT車用に `MAZDA_PARAM_MT = 2` を追加
- **CRZ_BTNS制御**: `MODE_X && MODE_Y`（メインボタン）の立ち上がりエッジで `controls_allowed` をトグル
- **キャンセル処理**: `CAN_OFF` で `controls_allowed = false` と `acc_main_on = false` を設定
- **ファイル**: [`panda/board/safety/safety_mazda.h`](panda/board/safety/safety_mazda.h)

#### 6. SET_P/SET_M ボタンイベント追加 (`interface.py`)
- **SET_P**: `accelCruise` ボタンイベントとして速度増加にマッピング
- **SET_M**: `decelCruise` ボタンイベントとして速度減少にマッピング
- **ファイル**: [`selfdrive/car/mazda/interface.py`](selfdrive/car/mazda/interface.py)

#### 7. 青信号アラート CEM依存解消 (`conditional_experimental_mode.py`)
- **変更**: `stop_sign_and_light()` をCEM条件判定の外で常時実行するよう変更
- **効果**: CEMがオフでも青信号アラートが機能する
- **ファイル**: [`frogpilot/controls/lib/conditional_experimental_mode.py`](frogpilot/controls/lib/conditional_experimental_mode.py)

#### 8. BrakePBClutchUI トグル追加 (`frogpilot_annotated_camera.h`)
- **機能**: ブレーキペダル・パーキングブレーキ・クラッチの状態をUIに表示するトグル
- **メソッド**: `paintBrakePBClutchStatus()` で描画
- **ファイル**: [`frogpilot/ui/qt/onroad/frogpilot_annotated_camera.h`](frogpilot/ui/qt/onroad/frogpilot_annotated_camera.h)

#### 検証に使用した実データ
- `Y:\Github\mazda2canbus\realdata` の rlog データ（43,135件のUDSメッセージ、359,174 CAN フレーム）

---

## 🔄 CI/CD パイプライン（自動ビルド・デプロイ）

本リポジトリでは、GitHub Actions を利用した **自動ビルド＆デプロイパイプライン** を構築しています。コードをプッシュするだけで、comma デバイス上で自動的にビルドが行われ、ビルド済みの成果物がリポジトリに反映されます。

### 対象ブランチ
以下のブランチにプッシュされた際、自動的にCIがトリガーされます。
- `feat-mazda2-dj-mt-frog`
- `test-mazda2-dj-mt-frog`
- `dev-mazda2-dj-mt-frog`

### 自動ビルドの流れ
1. **プッシュ → CIトリガー**: 対象ブランチにコードをプッシュすると、[Compile FrogPilot](.github/workflows/compile_frogpilot.yaml) ワークフローが自動起動します。
2. **セルフホストランナーでビルド**: comma デバイス（c3/c3x）自体がセルフホストランナーとして動作し、`/data/openpilot` 上で以下を実行します:
   - `git fetch` → `git reset --hard` で最新コードを取得
   - `poetry install` で依存関係を解決
   - `scons` でネイティブビルドを実行
3. **ビルド成果物をコミット**: ビルドが完了すると、成果物をコミット（メッセージ: `Compile FrogPilot [skip ci]`）し、`git push --force` でリポジトリに反映します。
4. **`[skip ci]` による無限ループ防止**: ビルドコミットには `[skip ci]` を付与しており、CI が再トリガーされるのを防ぎます。

### 手動実行（workflow_dispatch）
GitHub の Actions タブから手動でワークフローを実行することも可能です。以下のオプションを指定できます:
- **runner**: `c3` または `c3x` を選択
- **publish_frogpilot**: `FrogPilot` ブランチへプッシュ
- **publish_staging**: `FrogPilot-Staging` ブランチへプッシュ
- **publish_testing**: `FrogPilot-Testing` ブランチへプッシュ
- **update_translations**: 翻訳の自動更新を実行

### その他のワークフロー
| ワークフロー | 説明 |
|---|---|
| [`compile_frogpilot.yaml`](.github/workflows/compile_frogpilot.yaml) | メインのビルド＆デプロイ |
| [`schedule_update.yaml`](.github/workflows/schedule_update.yaml) | 定期スケジュールによる自動更新 |
| [`update_pr_branch.yaml`](.github/workflows/update_pr_branch.yaml) | PR ブランチの自動更新 |
| [`update_release_branch.yaml`](.github/workflows/update_release_branch.yaml) | リリースブランチの自動更新 |
| [`review_pull_request.yaml`](.github/workflows/review_pull_request.yaml) | PR の自動レビュー |

---

## 🔧 ロンジチューディナル制御（縦方向制御）のアーキテクチャ

Mazda2 DJ MT では、**experimental longitudinal mode** により縦方向（加減速）の制御を openpilot が担います。以下にその技術的な仕組みを説明します。

### レーダーECUの UDS プログラミングモードによる無効化

Mazda のストック ACC（MRCC）は **レーダーECU が縦方向を制御** する設計になっています。openpilot が縦方向を制御するには、このレーダーECU の制御を無効化する必要があります。

| 項目 | 詳細 |
|---|---|
| **レーダーECU アドレス** | `0x764`（CAN バス 0） |
| **無効化方式** | UDS 診断セッション制御（`0x10 0x02` → PROGRAMMING SESSION） |
| **セッション維持** | テスタープレゼント（`0x3E 0x80`）を約 2Hz で定期送信 |
| **参照実装** | [`longitudinal.py`](selfdrive/car/mazda/longitudinal.py) |

### なぜ「レーダーサポート: いいえ」なのか

レーダーECU を **PROGRAMMING SESSION** に移行させると、レーダーは通常の CAN メッセージ出力を停止します。これには以下が含まれます：

- **レーダートラックデータ**（`0x361`〜`0x366`：距離・角度・相対速度）
- **ACC 制御メッセージ**（`CRZ_CTRL` 等）

したがって、[`mazda_radar.dbc`](opendbc/mazda_radar.dbc) がリポジトリに存在しても、PROGRAMMING モード中はこれらのメッセージが流れないため **レーダーデータを取得できません**。これは [`interface.py`](selfdrive/car/mazda/interface.py) で `radarUnavailable = True` が設定されている理由です。

> **参考**: upstream の [commaai/opendbc#3355](https://github.com/commaai/opendbc/pull/3355)（yummydirtx による CX-5 2022 向け alpha longitudinal）でも全く同じアプローチを採用しており、"Radar remains unavailable while longitudinal is synthesized" と明記されています。

### 代わりの先行車検出: ビジョンベース

レーダーデータが利用できないため、openpilot の **ビジョンモデル**（カメラベース）が先行車の検出を担当します。これにより、レーダーなしでも追従制御が可能です。

### 合成 ACC メッセージの送信

レーダーECU の代わりに、openpilot が以下の合成メッセージを送信します：

| メッセージ | ID | 内容 |
|---|---|---|
| `CRZ_INFO` | `0x21B` | 加速度コマンド、ACC 状態、ストップ/レジュームビット |
| `CRZ_CTRL` | `0x21C` | クルーズ状態、先行車有無、距離設定、プロファイルテンプレート |

これらは Mazda の期待する状態遷移（ストップ＆ゴーのホールド/ラッチ/レジューム）をエミュレートするよう設計されています。詳細は [`longitudinal.py`](selfdrive/car/mazda/longitudinal.py) の `MazdaLongitudinalProfile` を参照してください。

### ⚠️ 制限事項

- **AEB（自動緊急ブレーキ）が無効化されます** — レーダーECU がプログラミングモードにあるため
- **ダッシュボードにレーダー関連の警告灯が表示される場合があります**（走行への影響は確認されていません）
- **FCW（前方衝突警告）も無効化されます**

### 🔮 将来の改善可能性

- comma デバイスで CAN ダンプを取得し、PROGRAMMING モード中にレーダートラックデータ（`0x361`〜`0x366`）が流れているか検証
- もし流れていれば、`RadarInterface` を実装してビジョン + レーダーの融合が可能になる（upstream PR #3355 の "Future Work" にも記載）
- 

## CI/CD ワークフロー

### ブランチ構成

| ブランチ | 内容 | 用途 |
|---|---|---|
| `test-mazda2-dj-mt-frog` | **ソースコード** | 開発者がpushする先 |
| `test-mazda2-dj-mt-frog-built` | **コンパイル済みバイナリ** | C3デバイスで実行する用 |

### ビルドフロー

1. 開発者が `test-mazda2-dj-mt-frog` にpush
2. C3デバイスのCIランナー（self-hosted）が自動的にビルドを実行
3. ビルド成果物を `test-mazda2-dj-mt-frog-built` にforce push

### C3デバイスでビルド済みバイナリを使用する

CIビルド完了後、以下のコマンドでビルド済みバイナリを取得:

```bash
cd /data/openpilot
git fetch origin test-mazda2-dj-mt-frog-built
git checkout test-mazda2-dj-mt-frog-built
# openpilotを再起動
```

### 注意事項

- CIランナーは常に `test-mazda2-dj-mt-frog` をcheckoutしておく必要があります（`get_branch` がローカルブランチ名を取得するため）
- ビルド完了後、C3デバイスで実行する時だけ `test-mazda2-dj-mt-frog-built` に切り替えます
- ソースとバイナリを分離することで、`git pull --rebase` 時のコンフリクトや `Unpacking objects` の問題を回避しています

---

## ログ記録メカニズム

### fingerprint認識とログ記録

fingerprintが認識されている場合でも未認識の場合でも、ログ記録の仕組みは同じです。イグニッションONで `loggerd` と `encoderd` が自動起動し、以下のデータが記録されます：

| データ | サービス名 | 内容 |
|--------|-----------|------|
| CANデータ | `can` | バス上の全CANメッセージ（100Hz） |
| CAN送信 | `sendcan` | openpilotから送信したCAN |
| カメラ映像 | `fcamera.hevc` | 道路カメラ（20fps） |
| ドライバー映像 | `dcamera.hevc` | ドライバーカメラ |
| センサー | `gyroscope`, `accelerometer` | IMUデータ |
| GPS | `gpsNMEA`, `gpsLocation` | 位置情報 |
| 車両状態 | `carState` | 速度、ハンドル等（100Hz） |
| 制御状態 | `controlsState` | openpilotの制御状態 |

### ログが記録されない条件

以下のいずれかの場合、ログ記録プロセス（loggerd, encoderd等）が停止します：

1. **「Disable Logging」がON** → FrogPilot設定 → Device Management で確認
2. **「Force Onroad」が有効** → 強制オンロード時は `no_logging = True` になる
3. **`DisableLogging` パラメータが設定**（notCar/bodyボットのみ）

### 3つの「録画」の違い

| 機能 | 対象 | トリガー | フォーマット |
|------|------|---------|-------------|
| **loggerd** | CAN/rlog/全センサーデータ | イグニッションONで自動 | capnproto + bzip2 |
| **encoderd** | カメラ映像（道路/広角/ドライバー） | イグニッションONで自動 | H.265/H.264 |
| **ScreenRecorder** | UI画面の動画 | 手動でボタン押下 | H.264（OMX） |

### デバッグモード

デバッグモードはUI表示の開発者メトリクス（FPS、メモリ使用量、CPU/GPU使用率等）を強制表示する機能です。rlogやCANのログ記録量には影響しません。画面録画ボタンが自動表示されるようになります。

### CANデータ等を確実に記録する手順

1. FrogPilot設定 → Device Management → **「Disable Logging」をOFF**にする
2. **「Force Onroad」を使用しない**（使用中は `no_logging = True` になる）
3. イグニッションON → 自動的に全データが記録される

### ログの保存先ディレクトリ

| 環境 | パス |
|------|------|
| **デバイス（comma 3X等）** | `/data/media/0/realdata/` |
| **HD設定あり** | `/data/media/0/realdata_HD/` |
| **PC（開発環境）** | `$HOME/.comma/media/0/realdata` |

#### セグメントディレクトリ構造

1セグメント = 60秒で自動的にローテーションされます。

```
/data/media/0/realdata/
└── 000001a3--c20ba54385/     ← 1回の走行（ルートディレクトリ）
    ├── --0/                  ← セグメント0（0〜60秒）
    │   ├── rlog              ← 全メッセージ（CAN、CarState、GPS等、capnproto形式）
    │   ├── qlog              ← rlogのサブセット（クイックアクセス用）
    │   ├── fcamera.hevc      ← 前方カメラ（HEVC / H.265、20fps）
    │   ├── ecamera.hevc      ← 広角カメラ（HEVC / H.265）
    │   ├── dcamera.hevc      ← ドライバーカメラ（RecordFront有効時のみ）
    │   └── qcamera.ts        ← 低品質前方カメラ（H.264、プレビュー用）
    ├── --1/                  ← セグメント1（60〜120秒）
    └── --2/                  ← セグメント2（120〜180秒）
```

**注意**: CANデータは個別ファイルではなく、`rlog` 内にcapnprotoメッセージとして格納されます。

---

## Mazda LKAS Fault 対策

### 問題の概要
Mazda車（GEN1）でopenpilot/FrogPilotによるステアリング制御中、ドライバーのハンドルトルク不足により車両EPSがLKAS_BLOCK信号を送信し、LKAS Fault（steerFaultPermanent）が発生する。一度Faultが発生すると車両再起動+1分待機が必要。

### 根本原因
1. ドライバーのハンドルトルク不足 → EPSがLKAS_BLOCK信号を送信（STEER_RATE 0x241）
2. LKAS_BLOCKの継続 → カメラモジュールがERR_BIT_1=1をセット（CAM_LKAS 0x243）
3. openpilotがERR_BIT_1をそのままEPSに転送 → EPSがエラー状態にロック
4. 車両再起動が必要になる

### CANメッセージフロー
```
カメラ(Bus 2) → panda(ブロック) → openpilot(読取り)
openpilot → CAM_LKAS送信(Bus 0) → EPS → STEER_RATE送信(Bus 0)
```
- pandaの`safety_mazda.h`でカメラのCAM_LKASはMain Busに転送されないようブロック
- openpilotだけがMain Bus（Bus 0）にCAM_LKASを送信

### 3層防御の実装

| 層 | ファイル | 変更内容 | 効果 |
|---|---|---|---|
| **予防層** | `carcontroller.py` | LKAS_BLOCK中はステアリング要求を0に | ERR_BIT_1への遷移を予防 |
| **伝播防止層** | `mazdacan.py` | `er1 = 0`（ERR_BIT_1を0に固定） | 車両のLKAS Fault警告灯を防止 |
| **検出緩和層** | `carstate.py` | `steerFaultPermanent = False` | 再起動不要に |

### 他プロジェクトの対応状況
- **上流commaai/openpilot**: LKAS Fault時は即時無効化+「Restart the Car」アラートのみ。回避策なし
- **MoreTore/openpilot**: TORQUE_INTERCEPTOR（ハードウェア）使用時に`steerFaultPermanent = False`。ソフトウェアワークアラウンドなし
- **全GEN1 Mazda車種共通**: DBC定義、検出ロジック、ステアリングパラメータは全車種同一

### 安全性のポイント
- LKAS_BLOCK（steerFaultTemporary）は引き続き正常に検出・処理される
- ERR_BIT_1はカメラモジュールの内部状態に過ぎず、LKAS_BLOCKが別経路で安全を担保
- 変更は最小限（3ファイル・各1-2行）で、openpilotのコアには影響しない

---

## ⚠️ 注意事項・免責

- **本ブランチの利用・改造・実車適用はすべて自己責任で行ってください。**
- 本プロジェクトは一切の動作保証を行いません。万一、車両や人身、第三者に損害が発生しても、開発者・貢献者は一切の責任を負いません。
- **必ずテスト環境で十分に検証し、安全が確保できる場合のみ実車でご利用ください。**

*本ブランチの開発内容は、将来的に本家 FrogPilot へのプルリクエストを予定しています。*

---

<div align="center" style="text-align: center;">

<h1>openpilot</h1>

<p>
  <b>openpilot is an operating system for robotics.</b>
  <br>
  Currently, it upgrades the driver assistance system in 300+ supported cars.
</p>

<h3>
  <a href="https://docs.comma.ai">Docs</a>
  <span> · </span>
  <a href="https://docs.comma.ai/contributing/roadmap/">Roadmap</a>
  <span> · </span>
  <a href="https://github.com/commaai/openpilot/blob/master/docs/CONTRIBUTING.md">Contribute</a>
  <span> · </span>
  <a href="https://discord.comma.ai">Community</a>
  <span> · </span>
  <a href="https://comma.ai/shop">Try it on a comma 3X</a>
</h3>

Quick start: `bash <(curl -fsSL openpilot.comma.ai)`

[![openpilot tests](https://github.com/commaai/openpilot/actions/workflows/selfdrive_tests.yaml/badge.svg)](https://github.com/commaai/openpilot/actions/workflows/selfdrive_tests.yaml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![X Follow](https://img.shields.io/twitter/follow/comma_ai)](https://x.com/comma_ai)
[![Discord](https://img.shields.io/discord/469524606043160576)](https://discord.comma.ai)

</div>

<table>
  <tr>
    <td><a href="https://youtu.be/NmBfgOanCyk" title="Video By Greer Viau"><img src="https://github.com/commaai/openpilot/assets/8762862/2f7112ae-f748-4f39-b617-fabd689c3772"></a></td>
    <td><a href="https://youtu.be/VHKyqZ7t8Gw" title="Video By Logan LeGrand"><img src="https://github.com/commaai/openpilot/assets/8762862/92351544-2833-40d7-9e0b-7ef7ae37ec4c"></a></td>
    <td><a href="https://youtu.be/SUIZYzxtMQs" title="A drive to Taco Bell"><img src="https://github.com/commaai/openpilot/assets/8762862/05ceefc5-2628-439c-a9b2-89ce77dc6f63"></a></td>
  </tr>
</table>


Using openpilot in a car
------

To use openpilot in a car, you need four things:
1. **Supported Device:** a comma 3/3X, available at [comma.ai/shop](https://comma.ai/shop/comma-3x).
2. **Software:** The setup procedure for the comma 3/3X allows users to enter a URL for custom software. Use the URL `openpilot.comma.ai` to install the release version.
3. **Supported Car:** Ensure that you have one of [the 275+ supported cars](docs/CARS.md).
4. **Car Harness:** You will also need a [car harness](https://comma.ai/shop/car-harness) to connect your comma 3/3X to your car.

We have detailed instructions for [how to install the harness and device in a car](https://comma.ai/setup). Note that it's possible to run openpilot on [other hardware](https://blog.comma.ai/self-driving-car-for-free/), although it's not plug-and-play.

------

<div align="center" style="text-align: center;">

<h1>FrogPilot 🐸</h1>

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/FrogAi/FrogPilot)
[![Discord](https://img.shields.io/discord/1137853399715549214?label=Discord)](https://discord.frogpilot.com)
[![Last Updated](https://img.shields.io/badge/Last%20Updated-February%2028th%2C%202025-brightgreen)](https://github.com/FrogAi/FrogPilot/releases/latest)
[![Wiki](https://img.shields.io/badge/Wiki-FrogPilot-blue?logo=wiki)](https://frogpilot.com/wiki/)

</div>

------

**FrogPilot** is a custom, community-driven, frog-themed fork of openpilot that grows and improves through the ideas and contributions of its users. It offers exciting new features and cutting-edge experiments that often arrive long before official releases. As an unofficial and highly experimental version of openpilot, **FrogPilot** should *always* be used with caution!

openpilot vs **FrogPilot**
------

#### Community
| Feature | openpilot | **FrogPilot** |
|---------|:---------:|:---------:|
| A Welcoming Community | ❌ | ✅ |
| Erich / Primary Moderators / 🦇 | ✅ | ❌ |

#### Core Features
| Feature | openpilot | **FrogPilot** |
|---------|:---------:|:---------:|
| Always On Lateral (Steering) | ❌ | ✅ |
| Blind Spot Integration | ✅ | ✅ |
| Conditional Experimental Mode | ❌ | ✅ |
| Custom Themes | ❌ | ✅ |
| Driver Monitoring | ✅ | ✅ |
| Driving Model Selector | ❌ | ✅ |
| Holiday Themes | ❌ | ✅ |
| Speed Limit Support | ❌ | ✅ |
| Weather Detection | ❌ | ✅ |

#### Device & Hardware
| Feature | openpilot | **FrogPilot** |
|---------|:---------:|:---------:|
| Advanced Volume Controller | ❌ | ✅ |
| Automatic Version Backups | ❌ | ✅ |
| C3 Support | ❌ | ✅ |
| comma Pedal Support | ❌ | ✅ |
| High Quality Recordings | ❌ | ✅ |
| SDSU Support | ❌ | ✅ |
| ZSS Support | ❌ | ✅ |

#### Gas/Brake
| Feature | openpilot | **FrogPilot** |
|---------|:---------:|:---------:|
| Adaptive Cruise Control (ACC) | ✅ | ✅ |
| Advanced Live Tuning | ❌ | ✅ |
| Custom Following Distances | ❌ | ✅ |
| Faster Human-Like Acceleration | ❌ | ✅ |
| Human-Like Speed Control in Curves | ❌ | ✅ |
| Smoother Human-Like Braking | ❌ | ✅ |

#### Steering
| Feature | openpilot | **FrogPilot** |
|---------|:---------:|:---------:|
| Advanced Live Tuning | ❌ | ✅ |
| Automatic Lane Changes | ❌ | ✅ |
| Increased Steering Torque* | ❌ | ✅ |
| Lane Centering (LKAS) | ✅ | ✅ |
| Lane Change Assist | ✅ | ✅ |

*Select vehicles only

And much much more!

🌟 Highlight Features
------

### 🚗 Always On Lateral (AOL)

With **"Always On Lateral"**, lane-centering stays active whenever cruise control is on, even when you press the accelerator or brake. This means steering assist won't cut out during manual speed adjustments giving you continuous support through curves, traffic, or mountain roads!

---

### 🧠 Conditional Experimental Mode (CEM)

**["Experimental Mode"](https://blog.comma.ai/090release/#experimental-mode)** lets openpilot drive at the speed it thinks a human would to allow slowing for curves, stopping at stoplights/stop signs, and adapting to traffic. This makes it powerful in complex scenarios, but it's still, well, "experimental" and less predictable than **"Chill Mode"**. But **"Conditional Experimental Mode"** gives you the best of both worlds by automatically switching between **"Chill Mode"** for steady cruising and **"Experimental Mode"** for more advanced situations to help fully automate your driving experience!

**"Conditional Experimental Mode"** switches into **"Experimental Mode"** when conditions like these are met:
- Approaching curves and turns
- Detecting slower or stopped lead vehicles
- Driving below a set speed
- Predicting an upcoming stop (e.g. stoplight or stop sign)

Once conditions clear it returns to **"Chill Mode"** for stability and predictability.

**Note: Stay attentive as "Experimental Mode" is an alpha feature and mistakes are expected!**

---

### 🎭 Driving Personalities

With **"Driving Personalities"**, you choose how the vehicle behaves with four adjustable profiles:

- **Traffic:** Catered towards stop-and-go traffic by minimizing gaps and delays  
- **Aggressive:** Aimed to provide tighter following distances and quicker reactions  
- **Standard:** Useful for a balanced, all-purpose driving  
- **Relaxed:** A smoother driving experience with larger following distance gaps  

Each profile can be fine-tuned to change the desired following distance, acceleration, and braking style letting you shape **FrogPilot**'s behavior to match your own driving preferences! Profiles can be switched instantly using the following distance button on the steering wheel, while **"Traffic Mode"** can be enabled by simply holding down the following distance button.

---

### 📏 Speed Limit Controller (SLC)

With **"Speed Limit Controller"**, **FrogPilot** automatically adapts to the road's posted speed using information from downloaded **["OpenStreetMap"](https://www.openstreetmap.org)** maps, online **["Mapbox"](https://www.mapbox.com)** data, and the vehicle's dashboard (if supported).

Offsets let you fine-tune how closely **FrogPilot** follows posted limits across different speed ranges allowing you to cruise slightly above or below for a more natural driving experience. If no speed limit is available, you can choose whether **FrogPilot** drives at the set speed, falls back to the last known speed limit, or uses **"Experimental Mode"** to estimate one with the driving model.

Maps can be downloaded directly in settings and updated automatically on a schedule ensuring your device always has the latest speed limits!

**Note: Speed limits are only as accurate as the available speed limit data. Always stay attentive and adjust your speed when necessary!**

---

### 🎨 Themes

With **"Themes"**, you can personalize **FrogPilot**'s driving screen to make it uniquely yours! Choose from:

- **Color Schemes**
- **Icon Packs**
- **Sound Packs**
- **Turn Signal Animations**
- **Steering Wheel Icons**

Enjoy pre-existing **FrogPilot** and seasonal holiday themes, or you can create your own with the **"Theme Maker"** and even share them with the community! For extra fun, enable features like the Mario Kart–style **"Rainbow Path"** or **"Random Events"** that add playful visual effects while you drive!

---

And lots more! From safety enhancements to personalization options, **FrogPilot** continues to evolve with features that put you in control. Check it out today for yourself!

---

🔧 Branches
------
| Branch                     | Install&nbsp;URL          | Description                                            | Recommended&nbsp;For     |
|----------------------------|---------------------------|--------------------------------------------------------|--------------------------|
| FrogPilot                  | frogpilot.download        | The main release branch.                               | Everyone                 |
| FrogPilot&#8209;Staging    | staging.frogpilot.download| Beta branch with upcoming features. Expect bugs!       | Early&nbsp;Adopters      |
| FrogPilot&#8209;Testing    | testing.frogpilot.download| Alpha branch with bleeding-edge features. Breaks often!| Advanced&nbsp;Testers    |
| FrogPilot&#8209;Development| No :)                     | Active development branch. Do not use!                 | **FrogPilot**&nbsp;Developers|
| MAKE&#8209;PRS&#8209;HERE  | No :)                     | Workspace for pull requests. Do not use!               | Contributors             |

🧰 How to Install
------

The easiest way to install **FrogPilot** is by entering this URL on the installation screen:

```
frogpilot.download
```

**DO NOT** install the **FrogPilot-Development** branch. I'm constantly breaking things on there, so unless you don't want to use **FrogPilot**, **NEVER** install it!

![](https://i.imgur.com/FsufQtO.png)

🐞 Bug Reports / Feature Requests
------

If you run into bugs, issues, or have ideas for new features, please post about it on the **[FrogPilot Discord](https://discord.gg/frogpilot)**! Feedback helps improve **FrogPilot** and create a better experience for everyone!

To report a bug, please post it in [**#bug-reports**](https://discord.com/channels/1137853399715549214/1162100167110053888).  
To request a feature, please post it in [**#feature-requests**](https://discord.com/channels/1137853399715549214/1160318669839147259).  

Please include as much detail as possible! Photos, videos, log files, or anything that can help explain the issue or idea are very helpful!

I'll do my best to respond promptly, but not every request can be addressed right away. Your feedback is always appreciated and helps make **FrogPilot** the best it can be!

📋 Credits
------

* [Aidenir](https://github.com/Aidenir)
* [AlexandreSato](https://github.com/AlexandreSato)
* [cfranyota](https://github.com/cfranyota)
* [cydia2020](https://github.com/cydia2020)
* [dragonpilot-community](https://github.com/dragonpilot-community)
* [ErichMoraga](https://github.com/ErichMoraga)
* [garrettpall](https://github.com/garrettpall)
* [jakethesnake420](https://github.com/jakethesnake420)
* [jyoung8607](https://github.com/jyoung8607)
* [mike8643](https://github.com/mike8643)
* [neokii](https://github.com/neokii)
* [OPGM](https://github.com/opgm)
* [OPKR](https://github.com/openpilotkr)
* [pfeiferj](https://github.com/pfeiferj)
* [realfast](https://github.com/realfast)
* [syncword](https://github.com/syncword)
* [twilsonco](https://github.com/twilsonco)

Star History
------

[![Star History Chart](https://api.star-history.com/svg?repos=FrogAi/FrogPilot&type=Date)](https://www.star-history.com/#FrogAi/FrogPilot&Date)

---
