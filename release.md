# 🍃  UnLeaf v1.1.1 — ProcessMonitor Hardening & CrashDump

Windows 11 / 10 向けゼロオーバーヘッド EcoQoS オプティマイザ **UnLeaf** のパッチリリース **v1.1.1** をリリースしました。
ETW ProcessMonitor のヘルスチェック堅牢性強化と、オプトイン式 CrashDump 機能の追加を含みます。**v1.1.0 ユーザーへのアップデートを推奨します。**

- README: [English](README_EN.md) | [日本語](README.md)
- 詳細な仕様はこちら: [Technical specifications](docs/Engine_Specification.md)

---

## ⚡ v1.1.1 の変更点 (What's New)

### 🔬 ProcessMonitor 堅牢性強化 (§9.15)

| # | 改善内容 |
|---|---------|
| 21 | **ヘルスチェック 3 段判定再設計** — ウォームアップ猶予 (120s) + lost event デルタ閾値 + `ControlTraceW(QUERY)` セッション生存確認 (1s キャッシュ)。idle (正常な無イベント) と ETW セッション死を正しく区別し、不要な ETW 再起動の連鎖を防止 |
| 22 | **`instance_` atomic 化 + TOCTOU 排除** — `std::atomic<ProcessMonitor*>` に変更。`EventRecordCallback` でローカル `self` にスナップショットし、acquire/release ペアリングで安全にルーティング |
| 23 | **ETW シャットダウン契約厳密化** — `stopMtx_` で Stop() vs IsHealthy() を直列化。5 ステップ順序 (`stopRequested_` → `CloseTrace` → `ControlTrace(STOP)` → `join` → `instance_ clear`) を DO NOT REORDER として明文化 |
| 24 | **TDH パース境界読みガード** — `ParseProcessStartEvent` の文字列プロパティを `propSize` 上限の bounded copy に置換。非終端 TDH ペイロードによる領域外読み (AV リスク) を排除 |
| 25 | **Start() 状態リセット** — 全ヘルスチェック状態を明示リセットし、サービス再起動後の残留状態汚染を防止 |

---

## ⚡ v1.1.0 の変更点

### 🛡️ メモリ安定化 — 長期稼働品質改善

| # | 改善内容 |
|---|---------|
| 1 | **trackedProcesses_ ハード上限制御** — 追跡プロセス数が上限 (2000) に達した際、zombie 優先・最古優先でプロセスを退避。`RemoveTrackedProcess()` への完全委任により Timer/WaitContext リークをゼロに抑制 |
| 2 | **eviction バースト対応** — 短時間に大量プロセスが起動した際、超過分を1パスで一括選出 (`SelectEvictionCandidates`) し、`partial_sort` で O(N log K) に最適化。4KB PMR arena により内部一時変数のヒープ割当を大幅削減 |
| 3 | **pendingRemoval ドロップ禁止** — 旧来の saturation 時 `pop()` (サイレントドロップ) を廃止。キューが溢れても push を継続し `LOG_ALERT` のみ出力。eviction 作業が失われない完全保証モデルへ移行 |
| 4 | **ドレイン負荷平準化** — `ProcessPendingRemovals()` を最大 256 件/tick に制限。処理スパイクを排除しコントロールループの応答性を維持。残留時は自動再スケジュール (SetEvent)、runaway backlog (> 8192) を `LOG_ALERT` で検知 |
| 5 | **Logger スタックバッファ最適化** — `WriteMessage()` の UTF-8 変換バッファをスタック固定長 (2048 bytes) に移行。通常パスのヒープ割当ゼロを達成 |
| 6 | **PMR fallback 可視化** — `ProcessEnforcementQueue` / `HandleSafetyNetCheck` / `SelectEvictionCandidates` の PMR arena fallback を Debug/Release 両方で検出・ログ出力。Release でも `LOG_INFO` で最大10回通知 |

### 🔒 コード品質・安全性

| # | 改善内容 |
|---|---------|
| 1 | **ODR 違反排除** — `CountingResource` クラスを anonymous namespace に一元定義。各関数内でのローカルクラス重複定義を廃止 |
| 2 | **ロック契約の実行時検証** — `SelectEvictionCandidate()` 先頭に DEBUG ASSERT を追加。`trackedCs_` 保持義務をアサーションで検証 |
| 3 | **pendingRemoval 3段階バックログ監視** — 50% → `LOG_INFO`、75% → `LOG_ALERT`、overflow → `LOG_ALERT` の多段警告体系 |

### 🏗️ RegistryPolicyManager v5 — レジストリポリシー完全再設計

| # | 改善内容 |
|---|---------|
| 7 | **IFEO / PowerThrottle 完全分離** — exe 名単位 (IFEO) とパス単位 (PowerThrottle) を独立管理。Chrome + Canary 等の同名複数パスを個別追跡。per-entry state machine (APPLYING → COMMITTED) で二重書き込みを排除 |
| 8 | **CPU 96.9% 暴走を修正** — `ProcessPendingRemovals` の無条件 `SetEvent` を `hasRemaining` 条件付きに変更。全 push サイト (P1-P3) を `wasEmpty` パターンに統一。EngineControlLoop にスピン検知 (連続 10,000 wakeup 超で `[SPIN DETECTED]` ログ + 1ms sleep) を追加 |
| 9 | **プロアクティブポリシー生成** — サービス起動時に config 全エントリへ事前ポリシー適用。プロセスが未起動の状態でも IFEO キーが有効。リアクティブ→プロアクティブへのアーキテクチャ移行 |
| 10 | **長パス対応・正規化統一** — `ResolvePathByHandle` を廃止し `CanonicalizePath` (GetFullPathNameW 2段階呼び出し) に統一。MAX_PATH 制限を撤廃。`NormalizePath` はログ/フォールバック用途に限定 |

### 🔧 name-only ターゲット PowerThrottle 遅延適用バグ修正

| # | 改善内容 |
|---|---------|
| 11 | **ETW フォールバックパス修正** — `ApplyOptimizationWithHandle` に name-only / path-based 分岐を追加。`chrome.exe=1` 等の name-only ターゲットは `HasPolicy` ゲートをバイパスし、パスが取得できた時点で即座に PowerThrottle ポリシーを適用。サービス起動後に起動したプロセスへの適用漏れを解消 |
| 12 | **SafetyNet ポリシー回復ループ** — プロセス起動直後に `QueryFullProcessImageNameW` が失敗した場合 (`needsPolicyRetry=true`)、10s 後の SafetyNet サイクルで自動リトライ。確実な適用を保証 |

### 🧠 ServiceEngine メモリ増加対策 (§9.14 Rev.17)

| # | 改善内容 |
|---|---------|
| 13 | **enforcementQueue_ 2キュー分離** — CRITICAL / NON-CRITICAL の2キュー (`std::deque`) に分離。TOTAL_LIMIT 絶対保証 + CRITICAL 512件/tick バースト制限で長期稼働時のメモリ線形増加を根絶 |
| 14 | **PendingRemoval 上限ガード** — CAS ベース上限ガード (MAX=512) + RAII NodeGuard。re-enqueue ループ廃止でキュー無限増大を根絶 |
| 15 | **タイマーハンドルリーク修正** — `ScheduleDeferredVerification` を `std::exchange` + `INVALID_HANDLE_VALUE` 同期で旧タイマーハンドル・コンテキストリークを根絶 |
| 16 | **SafetyNet 2パスラウンドロビン** — 2パスラウンドロビンスキャン + 30秒バックストップ追加。ETW silent drop 時も ≤30秒でリカバリ |

### 🧩 CrashDump オプトイン機能 (Phase 2)

| # | 改善内容 |
|---|---------|
| 17 | **MiniDump ライター新設** — `src/common/crash_handler.{h,cpp}` を新設。`SetUnhandledExceptionFilter` + `MiniDumpWithThreadInfo \| MiniDumpWithIndirectlyReferencedMemory` で全スレッドレジスタ・TEB・スタックから参照される小さなヒープ断片を含むダンプを出力 |
| 18 | **明示的 opt-in** — 既定無効。`UnLeaf.ini` の `[Logging] CrashDump=1` を明示設定した場合にのみインストールされ、`<install dir>\crash\` フォルダが生成される。ポータブル方針を堅持 |
| 19 | **WER 併用** — `EXCEPTION_CONTINUE_SEARCH` で Windows Error Reporting / デバッガにそのままチェインするため、UnLeaf MiniDump と WER の両方を取得可能 |
| 20 | **PDB 常時出力** — Release ビルドに `/Zi /DEBUG /OPT:REF /OPT:ICF` を適用し、`UnLeaf_Service.pdb` を常時出力。ダンプの完全シンボル化解析が可能 |

---

## 🚀 なぜ UnLeaf なのか? (技術的優位性)

PC チューニング系ツール (Process Lasso 等) の多くは、常時システム全体を監視する **「ポーリング (繰り返し調査)」** という古い設計に基づいています。これは常時 CPU サイクルを消費し、15〜20MB のメモリを無駄に食い潰します。ゲームのフレームレートを上げるためのツールが、逆にシステムを重くするという本末転倒な状況が生まれています。

**UnLeaf は、根本から異なります。**
Windows カーネルが提供する **ETW (Event Tracing for Windows)** と独自開発の「完全なイベント駆動アーキテクチャ」を採用しています。

* **待機中 CPU 0.00% の衝撃:** プロセスの起動・終了というイベントを直接捉え、仕事が終われば完全なスリープ (CPU 0%) に戻ります。
* **桁違いの省メモリ設計:** 純粋 C++ と Win32 API で直接組み上げられたコアは、24 時間の過酷なストレステストを経ながらも、わずか **3MB〜5MB** のメモリしか消費しません。
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

### UnLeaf Manager UI
<img width="546" height="439" alt="UnLeaf_v1 0 3" src="https://github.com/user-attachments/assets/51b88928-ddd7-4a3e-96d5-81021479d7b8" />

### UnLeaf promotional photos
<img width="2339" height="1536" alt="UnLeaf promotional photos" src="https://github.com/user-attachments/assets/f9d36565-315c-409e-9eb5-2b09e4b4e02f" />

### EcoQoS before and after images
<img width="1024" height="559" alt="EcoQoS before and after images" src="https://github.com/user-attachments/assets/02ae2b76-6eb1-4eca-83fc-aff15763ef15" />
