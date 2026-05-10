# FrogPilot (Mazda2 DJ MT カスタム版)

このリポジトリは、[yozorakumo/FrogPilot](https://github.com/yozorakumo/FrogPilot) による、**マツダ Mazda2 (DJ) 6MT モデル** への完全対応を行ったカスタムブランチです。

## 🌟 主な追加機能と修正点

本ブランチ (`FrogPilot`) では、マニュアルトランスミッション（MT）車両特有の挙動をサポートするために以下の実装を行っています。

### 1. ネイティブギアポジション認識
車両のネイティブ CAN 信号の解析により、高精度なギア判定を実現しました。
- **対象信号**: `0x165` (MT_CLUTCH) Byte 0
- **対応ギア**: 1速〜6速、ニュートラル（N）、リバース（R）すべてを正確に識別。
- **メリット**: RPM 比率による推定ではないため、クラッチ操作中や停車中も正確なギア段を表示可能です。

### 2. MT車最適化制御
- **クラッチ連動ディスエンゲージ**: クラッチペダルを踏み込んだ瞬間に、安全にオープンパイロットの制御を解除（ディスエンゲージ）します。
- **エンスト防止ロジック**: 縦方向制御において、MT 車の特性に合わせた加減速の調整を行っています。
- **冗長信号の活用**: `0x165` に加え、`0x9E` からもクラッチおよびニュートラルスイッチ情報を取得し、信頼性を高めています。

### 3. MT専用 UI 表示
- 走行中のギア数値をダッシュボードにリアルタイム表示します。
- クラッチが踏まれている状態（Clutch Pressed）を視覚的にフィードバックします。

### 4. 正確な車両識別 (Fingerprinting)
- Mazda2 DJ MT モデル固有の ECU（カメラ、レーダー、EPS、エンジン、ABS）のファームウェアバージョンをデータベースに登録。
- MT 車に存在しない TCM（トランスミッション制御モジュール）を定義から除外することで、迅速かつ正確な車両識別を可能にしました。

## 🚀 インストール方法 / Quick Start (Japanese)

実機（comma 3 / 3X 等）のセットアップ画面で、以下の URL を入力するだけでインストールが可能です。

1.  デバイスを **工場出荷状態 (Factory Reset)** にするか、ソフトウェアをアンインストールします。
2.  再起動後のセットアップ画面で **[Custom Software]** (またはカスタムURL入力) を選択します。
3.  以下の URL を入力します：
    ```
    smiskol.com/fork/yozorakumo/feat-mazda2-dj-mt-frog
    ```

### 💡 インストール時の注意点
- 安定した電源供給とネットワーク環境での作業を推奨します。
- もしインストールが失敗する場合は、ネットワーク接続やデバイスの空き容量を再度確認してください。

## 🛠 開発・調査の記録
今回の対応にあたって実施した CAN バス解析の詳細は、プロジェクト内の設計ドキュメントを参照してください。

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
