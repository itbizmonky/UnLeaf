# 🍃  UnLeaf v1.1.2 — Memory Leak Fix & Heap Fragmentation Reduction

Windows 11 / 10 向けゼロオーバーヘッド EcoQoS オプティマイザ **UnLeaf** のパッチリリース **v1.1.2** をリリースしました。
長時間稼働時に観測された Private Bytes の線形増加 (41 時間で約 9MB) を引き起こすメモリリークを修正します。**v1.1.1 以前のユーザーへのアップデートを強く推奨します。**

- README: [English](README_EN.md) | [日本語](README.md)
- 詳細な仕様はこちら: [Technical specifications](docs/Engine_Specification.md)

---

## ⚡ v1.1.2 の変更点 (What's New)

### 🐛 メモリリーク修正 (P0)

| # | 改善内容 |
|---|---------|
| 1 | **`jobObjects_` ハンドル蓄積を修正** — `RemoveTrackedProcess()` が `jobObjects_` のエントリを削除していなかったため、プロセス終了のたびに Windows Job Object ハンドルが蓄積し続けていた。`jobObjects_.erase(pid)` を `trackedCs_` 解放後・`jobCs_` 単独取得で実行するよう修正。ロック順序 (`jobCs_` → `trackedCs_`) を維持し lock inversion を回避 |

### ⚡ ヒープ断片化抑制 (P1/P2)

| # | 改善内容 |
|---|---------|
| 2 | **TDH バッファの `thread_local` 化** — `ParseProcessStartEvent()` がイベントごとに `std::vector<BYTE>` を alloc/free していた問題を解消。`thread_local` 再利用バッファ (`s_tdhBuffer`, `s_propBuffer`) に置換し、capacity を縮小しないことで反復ヒープ確保をゼロに削減 |
| 3 | **`wstringstream` → `to_wstring` 置換** — `ProcessMonitor` の 4 箇所のログサイトで `std::wstringstream` を `std::to_wstring()` に置換。一時 string オブジェクトの生成を最小化しヒープ断片化を抑制 |

### 🔍 デバッグ品質強化

| # | 改善内容 |
|---|---------|
| 4 | **`engineControlThreadId_` の `atomic<DWORD>` 化** — `DWORD` のまま他スレッドから参照した場合の C++ 可視性保証欠如を修正。`std::atomic<DWORD>` (`memory_order_relaxed`) に変更し、DEBUG アサート (`RemoveTrackedProcess`, `RefreshJobObjectPids`, `ProcessPendingRemovals`) の検出精度を 100% に向上。Release ビルドへの実行時コスト影響なし |

### 🧹 ヒープメモリ管理強化 (§9.17-B)

| # | 改善内容 |
|---|---------|
| 5 | **`HeapOptimizeResources` の適用** — `EngineCore::Start()` に `HeapSetInformation(GetProcessHeap(), HeapOptimizeResources, ...)` を追加。プロセスヒープが idle 時に未使用コミットページを積極的にデコミット (Windows 8.1+、失敗時は無視)。長時間稼働時のヒープ断片化による Private Working Set 成長を抑制 |

---

## 📊 修正効果

| 指標 | 修正前 (v1.1.1) | 修正後 (v1.1.2) |
|------|--------|--------|
| Private Bytes 増加率 | ~220 KB/時 (41h で約 9MB) | 停止 (収束) |
| Handle Count | 単調増加 | 収束 |
| ETW イベントあたりのヒープ確保 | 2 alloc/event | 0 alloc/event (thread_local) |
| メモリ増加率 (ヒープ断片化) | ~2 MB/時 (3→15.5 MB) | ~0.38 MB/時 (3.78→6.26 MB / 6.5h) ≈ 5倍改善 |

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

### UnLeaf Manager UI
<img width="546" height="439" alt="UnLeaf_v1 0 3" src="https://github.com/user-attachments/assets/51b88928-ddd7-4a3e-96d5-81021479d7b8" />

### UnLeaf promotional photos
<img width="2339" height="1536" alt="UnLeaf promotional photos" src="https://github.com/user-attachments/assets/f9d36565-315c-409e-9eb5-2b09e4b4e02f" />

### EcoQoS before and after images
<img width="1024" height="559" alt="EcoQoS before and after images" src="https://github.com/user-attachments/assets/02ae2b76-6eb1-4eca-83fc-aff15763ef15" />
