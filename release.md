# 🍃  UnLeaf v1.1.3 — ETW Monitoring Extended & EcoQoS Guarantee

Windows 11 / 10 向けゼロオーバーヘッド EcoQoS オプティマイザ **UnLeaf** のパッチリリース **v1.1.3** をリリースしました。
2026-04-26 の Windows 11 アップデート以降に観測された **EcoQoS 無効化の不安定化** (chrome.exe 子プロセスが時間経過とともに EcoQoS ON のまま残留する現象) を修正します。
ETW cold-dead 検知・3 ステート復帰機械の追加と、ETW 状態によらず EcoQoS を ≤ 20 秒以内に解除する根本要件対応を含みます。

- README: [English](README_EN.md) | [日本語](README.md)
- 詳細な仕様はこちら: [Technical specifications](docs/Engine_Specification.md)

---

## ⚡ v1.1.3 の変更点 (What's New)

### 🐛 SafetyNet ラウンドロビン PID 張り付き修正 (#1)

| # | 改善内容 |
|---|---------|
| 1 | **`lastScannedPid_` 強制リセット** — `ScanRunningProcessesForMissedTargets` の全走査完了時 (pass1 自然終了 / pass2 完了) に `lastScannedPid_=0` を強制セット。次 tick で必ず PID 0 から再走査し、高 PID 領域への継続的な張り付きによる低 PID ターゲット (早期起動の chrome.exe 等) の取りこぼしを防止 |

### 🔄 周期 InitialScan 短縮 — 根本要件対応 (#2)

| # | 改善内容 |
|---|---------|
| 2 | **`PERIODIC_FULL_SCAN_INTERVAL` 60 s → 20 s** — 「EcoQoS を OFF にする」という根本要件は ETW 状態によらず保証すべき。ETW が破損している間 (cold-dead 検知前の最大 4 分間) でも、NORMAL モードの `InitialScan()` が 20 秒ごとに発火し新規 Chrome EcoQoS を最大 20 秒以内に解除する。リソース影響: ToolHelp32 scan ~1-5 ms × 3 回/分 → 追加 CPU ≈ 0.007 %、メモリ増加なし (Snapshot は都度解放) |
| 2b | **`DEGRADED_SCAN_INTERVAL` 30 s → 20 s** — DEGRADED_ETW モード中の `InitialScanForDegradedMode` 周期も短縮し、全モードで検知遅延 ≤ 20 s を統一 |

### 🔍 ETW Cold-Dead 検知 + 3 ステート復帰機械 (#4+#5 拡張)

| # | 改善内容 |
|---|---------|
| 3 | **ETW cold-dead 検知** — 従来の hot stall 検知 (eventCount ≥ 100、delta = 0) に加え、**cold-dead 検知** (起動/再起動後 eventCount = 0 が `ETW_COLD_DEAD_THRESHOLD_MS = 240 s` 継続 + ターゲット稼働中) を追加。`IsHealthy()` は `ControlTrace(QUERY)` 成功なら healthy と誤判定するため cold dead を見逃す盲点を補完 |
| 4 | **`EtwState` 3 ステート機械** (`HEALTHY` / `VERIFYING_RECOVERY` / `DEGRADED`) — 再起動後は `VERIFYING_RECOVERY` に遷移し `ETW_RECOVERY_VERIFY_MS = 30 s` 以内に eventCount 増加を確認。増加なければ最大 2 回リトライ後 `DEGRADED_ETW` 遷移。`etwVerificationBaseCount_` で Start() 直後の基準値を固定し、hotStall 後の `lastEtwEventCount_` 汚染による偽陰性を回避 |
| 5 | **`RestartETW()` ヘルパー** — `ProcessMonitor.Stop() → Sleep(50 ms) → Start()` + state 更新を集約。Sleep(50 ms) で ETW セッション teardown race を防止 |

### 📊 診断ログ強化 (#3)

| # | 改善内容 |
|---|---------|
| 6 | **`[DIAG]` ログに `etwEvents=N` 追加** — 既存の 60 秒周期 `[DIAG]` 診断行に ETW 累計イベント数フィールドを追加。ETW デリバリが正常に機能しているかをログから直接検証可能に |

---

## 🛡️ 障害ケース別の対処時間 (変更後)

| 障害パターン | 最大検知〜補正時間 | 担当機構 |
|---|---|---|
| SafetyNet PID 張り付き (chrome 低 PID 取りこぼし) | 〜30 秒 | #1 lastScannedPid_ リセット |
| ETW silent drop (chrome イベントのみ不着) | **〜20 秒** | #2 周期 InitialScan (20 s) |
| ETW stall (session alive だが events 増加なし) | 〜30 秒 + 3 分 cooldown | #4+#5 hot stall 検知 |
| ETW cold dead (起動直後から events なし) | 〜240 秒 + 30 s×2 verify | #4+#5 cold dead 検知 |
| ETW 完全停止 (session dead) | 〜90 秒 | 既存 `IsHealthy()` 機構 |
| 新 Chrome (ETW 破損中) | **〜20 秒** | #2 PERIODIC_FULL_SCAN |

---

## 📊 EcoQoS 検知遅延保証 (変更後)

| ETW 状態 | 新規 Chrome EcoQoS 最大遅延 |
|---------|---------------------------|
| ETW 正常 | ≒ 0ms（ETW リアルタイム）|
| ETW 破損（cold-dead 検知前の最大 4 分間） | **≤ 20 秒**（PERIODIC_FULL_SCAN 20 s） |
| DEGRADED_ETW | **≤ 20 秒**（InitialScanForDegradedMode 20 s） |

---

## 📊 修正効果

本バージョンで修正される主症状:

- **症状 A**: OS 起動後しばらくは機能するが、時間経過とともに chrome.exe 子プロセスに EcoQoS ON が残留する → `#1` + `#2` で対処
- **症状 B**: OS 起動直後に EcoQoS 解除が全く機能せず、ログモード切替等の操作で正常化する → `#4+#5 cold dead` で対処
- **症状 C** (新規): ETW が破損している 4 分間、新規 Chrome の EcoQoS 解除が最大 60 秒遅延する → `#2 PERIODIC_FULL_SCAN 20 s` で ≤ 20 秒に短縮

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
