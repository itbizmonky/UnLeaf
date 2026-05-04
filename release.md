# 🍃  UnLeaf v1.1.4 — ETW Callback Non-Blocking Fix

Windows 11 / 10 向けゼロオーバーヘッド EcoQoS オプティマイザ **UnLeaf** のパッチリリース **v1.1.4** をリリースしました。
サービス実行中に発生した **ACCESS VIOLATION クラッシュ**（Chrome 起動時に `AssignProcessToJobObject` が ETW コールバックスレッドをブロックし、最大 9 分の EngineControlLoop 停止 → `INVALID_PROCESSTRACE_HANDLE` 参照）を根本修正します。

- README: [English](README_EN.md) | [日本語](README.md)
- 詳細な仕様はこちら: [Technical specifications](docs/Engine_Specification.md)

---

## ⚡ v1.1.4 の変更点 (What's New)

### 🐛 ETW コールバック非ブロッキング化 — クラッシュ根本修正

#### 障害の概要

| 項目 | 内容 |
|------|------|
| 例外コード | `0xC0000005` ACCESS_VIOLATION (READ) |
| フォールトアドレス | `0xFFFFFFFFFFFFFFFF` = `INVALID_PROCESSTRACE_HANDLE` |
| 影響バージョン | v1.1.3 以前 |
| 再現条件 | Chrome 起動時に ETW 再起動が重なった場合 |

#### 根本原因

`OnProcessStart` (ETW コールバック) が `ApplyOptimization` を同期呼び出しし、`AssignProcessToJobObject` が Chrome の sandbox job object 管理と競合してカーネルレベルで長時間待機。ブロック中は `consumerThread_.join()` が `EngineControlLoop` を完全停止させ、2 回目の ETW 再起動時に `INVALID_PROCESSTRACE_HANDLE` がポインタとして参照されクラッシュ。

#### 修正内容

| # | 変更 |
|---|------|
| 1 | **`OnProcessStart` 非ブロッキング化** — `ApplyOptimization` 直接呼び出しを廃止。`IsTargetName` / `IsTrackedParent` / `HasPathTargets` の軽量フィルタのみ実行し、`EnqueueRequest(ETW_PROCESS_START)` でキューに積んで即リターン。ブロッキング OS 呼び出し (`OpenProcess`, `CreateJobObject`, `AssignProcessToJobObject` 等) はすべて `EngineControlLoop` 上で実行 |
| 2 | **`EnforcementRequest` 構造体拡張** — `parentPid` / `imageName` / `imagePath` フィールドを追加。`ETW_PROCESS_START` 専用コンストラクタを追加。既存コンストラクタとの互換性を維持 |
| 3 | **`DispatchEnforcementRequest` に `ETW_PROCESS_START` 分岐追加** — `trackedProcesses_.find` の前（lock 外）で `ETW_PROCESS_START` を処理。`IsTrackedParent` / `IsTargetName` / `HasPathTargets` を再チェックし `ApplyOptimization` / `TryApplyByPath` を呼ぶ |
| 4 | **`queueCs_` 設計契約コメント** — `// ZERO-I/O, no blocking — deque ops only` を宣言に付与し、将来の設計逸脱を防止 |

### 🔧 ETW 安定性改善 (Windows 11 Build 26200)

| # | 変更 |
|---|------|
| 5 | **`MatchAnyKeyword=0x30` バグ修正** — `Microsoft-Windows-Kernel-Process` プロバイダが `keyword=0` でイベントを発行するため、`MatchAnyKeyword=0x30` (PROCESS\|THREAD) がすべてのコールバックを無音でブロックしていた。`MatchAnyKeyword=0` に変更し ETW デリバリを回復 |
| 6 | **ETW バッファ定数拡張** — `ETW_BUFFER_SIZE_KB` 64→128 KB、`ETW_MIN_BUFFERS` 4→8、`ETW_MAX_BUFFERS` 32→64 (`MatchAnyKeyword=0` による高イベント量 ~1,200/sec への対応) |
| 7 | **ConsumerThread 診断ログ追加** — `OpenTraceW` ハンドル値、`ProcessTrace` 入退出・経過時間、初回コールバック受信確認、初回イベント keyword/eventId を記録 |
| 8 | **`ResolveProcessPath` ノイズ抑制** — `error=31` (システムプロセス) / `error=87` (終了済みプロセス) を `QueryFullProcessImageNameW` デバッグ出力から除外 |
| 9 | **ログレベル正規化** — `OpenTraceW handle=` / `ProcessTrace enter/exit` を ALERT→INFO に、`Lost event detected` を ALERT→DEBUG に変更 (このプラットフォームでは約 1/sec の構造的ノイズと確認済み) |

---

## 🛡️ 修正効果

| 障害ケース | v1.1.3 | v1.1.4 |
|---|---|---|
| Chrome 起動時 `AssignProcessToJobObject` ブロック | EngineControlLoop 停止 → クラッシュ | ETW callback 即リターン、EngineControlLoop 継続 |
| ETW 再起動中の `INVALID_PROCESSTRACE_HANDLE` 参照 | ACCESS VIOLATION | 発生しない (callback が blocking しない) |
| `consumerThread_.join()` 無期限ブロック | 発生 (最大 9 分) | 発生しない |

---

## 📊 EcoQoS 検知遅延保証 (v1.1.4 時点)

| ETW 状態 | 新規 Chrome EcoQoS 最大遅延 |
|---------|---------------------------|
| ETW 正常 | ≒ 0ms（ETW リアルタイム） |
| ETW 破損（cold-dead 検知前の最大 4 分間） | **≤ 20 秒**（PERIODIC_FULL_SCAN 20 s） |
| DEGRADED_ETW | **≤ 20 秒**（InitialScanForDegradedMode 20 s） |

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
