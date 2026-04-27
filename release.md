# 🍃  UnLeaf v1.1.3 — ETW Monitoring Stability Fix

Windows 11 / 10 向けゼロオーバーヘッド EcoQoS オプティマイザ **UnLeaf** のパッチリリース **v1.1.3** をリリースしました。
2026-04-26 の Windows 11 アップデート以降に観測された **EcoQoS 無効化の不安定化** (chrome.exe 子プロセスが時間経過とともに EcoQoS ON のまま残留する現象) を修正します。

- README: [English](README_EN.md) | [日本語](README.md)
- 詳細な仕様はこちら: [Technical specifications](docs/Engine_Specification.md)

---

## ⚡ v1.1.3 の変更点 (What's New)

### 🐛 SafetyNet ラウンドロビン PID 張り付き修正 (#1)

| # | 改善内容 |
|---|---------|
| 1 | **`lastScannedPid_` 強制リセット** — `ScanRunningProcessesForMissedTargets` の全走査完了時 (pass1 自然終了 / pass2 完了) に `lastScannedPid_=0` を強制セット。次 tick で必ず PID 0 から再走査し、高 PID 領域への継続的な張り付きによる低 PID ターゲット (早期起動の chrome.exe 等) の取りこぼしを防止 |

### 🔄 周期 InitialScan 追加 (#2)

| # | 改善内容 |
|---|---------|
| 2 | **60 秒周期の全プロセス再スキャン** — NORMAL モード時に `InitialScan()` を 60 秒ごとに自動発火 (`PERIODIC_FULL_SCAN_INTERVAL=60000`)。SafetyNet ラウンドロビン (10s × 64 PID/tick) が ~30〜50s で全 PID をインクリメンタルにカバーするため、InitialScan は ETW 欠損時の descendant 追跡強制補完として機能する |

### 🔍 ETW stall 検知 + 自動再起動 (#4 + #5)

| # | 改善内容 |
|---|---------|
| 3 | **ETW stall 検知** — 30 秒間 ETW イベントカウントが増加せず (`ETW_STALL_CHECK_INTERVAL=30s`)、かつターゲットプロセスが稼働中の場合に ETW session dead を検知。`ProcessMonitor` を自動停止→再起動。3 分クールダウン (`ETW_RESTART_COOLDOWN_MS=180s`) により再起動ループを防止。再起動失敗時は `DEGRADED_ETW` モードに遷移 |

### 📊 診断ログ強化 (#3)

| # | 改善内容 |
|---|---------|
| 4 | **`[DIAG]` ログに `etwEvents=N` 追加** — 既存の 60 秒周期 `[DIAG]` 診断行に ETW 累計イベント数フィールドを追加。ETW デリバリが正常に機能しているかをログから直接検証可能に |

---

## 🛡️ 障害ケース別の対処時間

| 障害パターン | 最大検知〜補正時間 | 担当機構 |
|---|---|---|
| SafetyNet PID 張り付き (chrome 低 PID 取りこぼし) | 〜30 秒 | #1 lastScannedPid_ リセット |
| ETW silent drop (chrome イベントのみ不着) | 〜60 秒 | #2 周期 InitialScan |
| ETW stall (session alive だが events 増加なし) | 〜30 秒 + 3分 cooldown | #4+#5 stall 検知 & 再起動 |
| ETW 完全停止 (session dead) | 〜90 秒 | 既存 `IsHealthy()` 機構 |

---

## 📊 修正効果

本バージョンで修正される主症状:

- **症状 A**: OS 起動後しばらくは機能するが、時間経過とともに chrome.exe 子プロセスに EcoQoS ON が残留する → `#1` + `#2` で対処
- **症状 B**: OS 起動直後に EcoQoS 解除が全く機能せず、ログモード切替等の操作で正常化する → `#4+#5` で対処

---

## 🚀 なぜ UnLeaf なのか? (技術的優位性)

PC チューニング系ツール (Process Lasso 等) の多くは、常時システム全体を監視する **「ポーリング (繰り返し調査)」** という古い設計に基づいています。これは常時 CPU サイクルを消費し、15〜20MB のメモリを無駄に食い潰します。ゲームのフレームレートを上げるためのツールが、逆にシステムを重くするという本末転倒な状況が生まれています。

**UnLeaf は、根本から異なります。**
Windows カーネルが提供する **ETW (Event Tracing for Windows)** と独自開発の「完全なイベント駆動アーキテクチャ」を採用しています。

* **待機中 CPU 0.00% の衝撃:** プロセスの起動・終了というイベントを直接捉え、仕事が終われば完全なスリープ (CPU 0%) に戻ります。
* **省メモリ設計:** 純粋 C++ と Win32 API で直接組み上げられたコアは、1 週間の過酷なストレステストを経ながらも、**約 15MB** のメモリしか消費しません。
* **ミリ秒の暗殺者:** Chrome などが子プロセス (タブ) を生成した瞬間を OS レベルで検知し、ラグなく最大限の EcoQoS を叩き込みます。

---

## 🔓 オープンコア・フィロソフィー (The Open Core Model)

UnLeaf は、**「コアエンジン (Service) は完全なオープンソース、設定用 UI (Manager) はクローズドソース」** というオープンコアモデルを採用しています。

**なぜエンジンをオープンにするのか?**
バックグラウンドで `SYSTEM` 権限として常駐し、プロセスを監視するソフトウェアには「絶対的な透明性と信頼」が不可欠です。世界中のハッカーやエンジニアがソースコードを読み、その驚異的な軽さと安全性を自らの目で確認し、さらには自分でビルドできるようにしています。

設定用の高機能 UI (UnLeaf Manager) は、独自の機能を提供するための独自プロプライエタリなソフトウェアとして、ビルド済みのバイナリ (exe) にのみ組み込んで配布しています。

---

## 📦 一般ユーザーの方へ (インストール方法)

ソースコードをビルドする必要はありません。今すぐ使い始めることができます。

1. [Releases](../../releases) ページにアクセスします。
2. 最新の `UnLeaf_v1.x.x.zip` をダウンロードし、任意のフォルダに解凍します。
3. `UnLeaf_Manager.exe` を実行します (初回のみ、サービス登録のために管理者権限の確認ダイアログが出ます)。
4. 最適化したいアプリ (例: `discord.exe`, `obs64.exe`) をリストに追加し、「Start Service」を押すだけです。
5. あとは Manager を閉じても、静かなエンジンが常にあなたの PC を守り続けます。

---

## 🛠️ 開発者・ギーク向け (ビルド手順)

本リポジトリには、UnLeaf の心臓である `UnLeaf_Service` の完全なソースコードが含まれています。提供されている `CMakeLists.txt` はオープンコア部に完全対応しており、クローンしてきた直後にエンジン単体をビルド可能です。

### 前提条件
* Windows 10 / 11 SDK
* CMake (3.20 以上)
* MSVC Compiler (Visual Studio 2022 を推奨)

### ビルドコマンド
```powershell
git clone https://github.com/itbizmonky/UnLeaf.git
cd UnLeaf
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### テスト実行
```powershell
ctest --test-dir build -C Release --output-on-failure
# Expected: 151/151 tests passed
```

---

**Build Note:**
- Windows Native C++ / Compiled with MSVC
- SHA-256: (TBD)

---

### UnLeaf promotional photos
<img width="1376" height="768" alt="Gemini_Generated_Image_wg9r5cwg9r5cwg9r" src="https://github.com/user-attachments/assets/a7c0f3eb-866c-43f8-b421-36df078e5fb9" />

### Comparison of UnLeaf before and after use
![unleaf_before_after](https://github.com/user-attachments/assets/3d35e6d2-548d-4bc7-8678-946ec2a2c05a)

### UnLeaf Manager UI
<img width="546" height="439" alt="スクリーンショット 2026-04-12 175809" src="https://github.com/user-attachments/assets/29f6411c-2061-4a15-b5cd-81db993ad608" />

### EcoQoS before and after images
<img width="1024" height="559" alt="EcoQoS before and after images" src="https://github.com/user-attachments/assets/02ae2b76-6eb1-4eca-83fc-aff15763ef15" />
