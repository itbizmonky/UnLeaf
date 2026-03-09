[English](README_EN.md) | [日本語](README.md)

[![Build](https://github.com/itbizmonky/UnLeaf/actions/workflows/build.yml/badge.svg)](https://github.com/itbizmonky/UnLeaf/actions/workflows/build.yml)

# 🍃 UnLeaf - The Zero-Overhead EcoQoS Optimizer

**UnLeaf**は、Windows 11 / 10 環境において、指定したアプリケーションの「EcoQoS（効率モード）」と「電力スロットリング」をOSの深淵から完全に無効化する、究極のバックグラウンド最適化ツールです。

Originally created by kbn.

---

## 🔥 なぜ UnLeaf なのか？ (圧倒的な技術的優位性)

PCチューニング界隈で長年使われてきた既存のツール（Process Lasso等）は、数秒おきにシステム全体を見回る「ポーリング（定期監視）」という古い設計に基づいています。これは常にCPUサイクルを消費し、15〜20MBのメモリを無駄に食い続けます。ゲームのフレームレートを上げるためのツールが、実はシステムを重くしているというジレンマがありました。

**UnLeafは、根本から異なります。**
Windowsカーネルの **ETW (Event Tracing for Windows)** と直接連携する「完全なイベント駆動アーキテクチャ」を採用しています。

* **待機時CPU 0.00% の衝撃:** プロセスが生成・終了した「その数ミリ秒」だけ目を覚まし、仕事が終われば再び完全なスリープ（CPU 0%）に戻ります。
* **極限の省メモリ設計:** モダンC++とWin32 APIで直接組み上げられたコアは、長時間の過酷なストレステストを経てもメモリリークを一切起こさず、わずか **3MB〜5MB** のメモリしか消費しません。
* **ミリ秒の暗殺者:** 対象アプリが子プロセスを生成した瞬間をOSレベルで検知し、ラグなしで即座にEcoQoSを剥がし取ります。

---

## 設計思想: "Set and Forget"

- **インストール後は完全自動**。手動操作は不要です
- **イベント駆動**: プロセスの起動・スレッド生成を ETW (Event Tracing for Windows) で検知し、即座に対処します。何もイベントがなければ CPU を一切使いません
- **3フェーズ適応制御**: プロセスの状態に応じて AGGRESSIVE → STABLE → PERSISTENT を自動切替し、最小限のリソースで最大の効果を維持します
- **安全第一**: システム重要プロセス (csrss.exe, lsass.exe, svchost.exe 等) は保護リストで保護されており、操作対象になりません

---

## 何をするか

- ターゲットプロセスの EcoQoS (効率モード) を無効化
- プロセス優先度を HIGH_PRIORITY_CLASS に設定
- スレッドの Power Throttling を無効化
- レジストリポリシー (Image File Execution Options) で EcoQoS 適用を抑制

## 何をしないか

- オーバークロックやハードウェア設定の変更
- チート・ゲーム改造
- ネットワーク通信 (完全オフライン動作)
- 個人情報の収集・送信
- ターゲット以外のプロセスへの干渉

---

## 動作要件

| 項目 | 要件 |
|------|------|
| OS | Windows 10 (1709 以降) / Windows 11 |
| 権限 | 管理者権限 |
| メモリ | 約 2MB |
| ディスク | 約 1.2MB |

> **補足**: Windows 11 では `NtSetInformationProcess` (NT API) を優先使用し、Windows 10 では `SetProcessInformation` (Win32 API) で同等の制御を行います。OS バージョンは起動時に自動判定されます。

---

## インストール方法 (バイナリ ZIP)

ソースコードをビルドする必要はありません。直感的なUIですぐに使い始めることができます。
1. Releases ページにアクセスします。
2. 最新の UnLeaf_v1.x.x.zip をダウンロードし、任意のフォルダに解凍します。
3. `UnLeaf_Manager.exe` を実行します（※初回のみ、サービス登録のために管理者権限の確認ダイアログが出ます）。
4. 最適化したいアプリ（例: discord.exe）をリストに追加し、「Start Service」を押すだけです。
5. あとはManagerを閉じても、不可視のエンジンが永遠にあなたのPCを保護し続けます。

> **補足**: ⚠️ インストール時のセキュリティ警告について<br>本ソフトウェアは個人開発のフリーソフトであり、高価なコードサイニング証明書（デジタル署名）を取得していません。そのため、ダウンロード時や初回実行時に Windows Defender の SmartScreen 画面（「Windows によって PC が保護されました」という青い画面）が表示されることがあります。<br>これは未知のファイルに対する標準的な警告です。本ソフトの安全性は公開されているソースコードによって担保されています。警告が出た場合は、「詳細情報」をクリックし、「実行」ボタンを押してインストールを続行してください。

---

## アンインストール

### 推奨: Manager UI からのアンインストール (完全)

1. `UnLeaf_Manager.exe` を管理者として起動
2. 「サービス登録解除 (Unregister)」をクリック
3. UnLeaf フォルダを削除

> Manager UI からの登録解除は、レジストリの完全なクリーンアップを保証します。
> `RemoveAllPolicies` は冪等 (idempotent) に設計されており、マニフェスト不存在・レジストリキー不存在・
> 内部状態が空の場合でもすべて正常系として安全に完了します。

---

## UnLeaf が変更するレジストリ

UnLeaf は以下の **2箇所のみ** をレジストリに書き込みます。これ以外のレジストリキーには一切触れません。

| # | パス | 値 | 目的 |
|---|------|-----|------|
| 1 | `HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerThrottling\<FullExePath>` | DWORD `1` | EcoQoS 永続除外 |
| 2 | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<ExeName>\PerfOptions\CpuPriorityClass` | DWORD `3` | CPU 優先度 High |

> **保証**: サービス停止時およびサービス登録解除時に、上記エントリはすべて自動削除されます。

### レジストリのライフサイクル

| 操作 | レジストリ | 説明 |
|------|-----------|------|
| プロセス追加・削除・無効化 | 変化なし | Manager UI の操作ではレジストリを変更しません |
| サービス停止 | **全削除** | そのセッションで適用したエントリをすべて削除します |
| サービス登録解除 | **全削除** | マニフェストファイル経由で過去の全エントリを削除します (クラッシュ後も安全) |

---

## 想定ユースケース

| アプリケーション | 効果 |
|-----------------|------|
| Chrome / Edge | バックグラウンドタブの throttle を防止 |
| ゲーム全般 | Alt-Tab 後のパフォーマンス低下を防止 |
| DAW (Cubase, FL Studio 等) | レンダリング中の CPU throttle を防止 |
| 動画編集 (DaVinci Resolve 等) | エンコード処理の速度低下を防止 |

---

## FAQ

### EcoQoS とは？
Windows 10 (1709) で Power Throttling として導入された省電力 API です。OS がプロセスの CPU 周波数とスケジューリング優先度を引き下げることで電力消費を抑えます。Windows 11 ではタスクマネージャーに「効率モード」(葉っぱアイコン) として視覚的に表示されます。

### CPU 使用率は上がりませんか？
**上がりません。** UnLeaf はイベント駆動で動作しており、監視対象のイベントがないときは `WaitForMultipleObjects(INFINITE)` で完全にスリープしています。アイドル時の CPU 使用率は 0% です。

### セキュリティリスクはありますか？
以下のセキュリティ対策を実装しています:
- **DACL**: IPC 通信は SYSTEM + Administrators のみアクセス可能
- **入力バリデーション**: プロセス名のパストラバーサル検査、長さ制限
- **保護リスト**: csrss.exe, lsass.exe, svchost.exe 等のシステム重要プロセスは操作対象外
- **権限分離**: コマンドごとに PUBLIC / ADMIN / SYSTEM_ONLY の権限レベル

### Windows 10 で使えますか？
**はい、動作します。** Windows 10 (1709 以降) では `SetProcessInformation` による Power Throttling 制御を使用し、Windows 11 では `NtSetInformationProcess` を優先しつつ `SetProcessInformation` にフォールバックします。OS バージョンは起動時に自動判定されるため、ユーザー側の設定は不要です。

### 設定ファイルはどこにありますか？
`UnLeaf.ini` が `UnLeaf_Service.exe` と同じフォルダに生成されます。テキストエディタで直接編集でき、保存すると自動的にリロードされます (サービス再起動は不要)。

```ini
; UnLeaf Configuration
[Logging]
LogLevel=INFO
LogEnabled=1

[Targets]
chrome.exe=1
```

### サービスが動いているか確認するには？
`UnLeaf_Manager.exe` を起動し、UIからサービスの状態を確認できます。コマンドラインからは `sc query UnLeafService` でも確認可能です。

---

## 技術アーキテクチャ

### イベント駆動制御ループ (WFMO)

Engine control thread は `WaitForMultipleObjects(INFINITE)` で 5 つのハンドルを監視し、イベントがないときは CPU を一切消費しません。

| Index | ハンドル | トリガー | 処理 |
|-------|---------|----------|------|
| 0 | `stopEvent_` | サービス停止要求 | ループ脱出 |
| 1 | `configChangeHandle_` | `FindFirstChangeNotification` (INI 変更) | デバウンス後にコンフィグリロード + ターゲット再構築 |
| 2 | `safetyNetTimer_` | Waitable Timer (10 秒周期) | STABLE フェーズのプロセスのみ EcoQoS 再適用チェック |
| 3 | `enforcementRequestEvent_` | ETW コールバック / タイマーからの `EnqueueRequest()` | キューを swap → `DispatchEnforcementRequest()` で逐次処理 |
| 4 | `hWakeupEvent_` | `OnProcessExit` コールバックからの wakeup | `ProcessPendingRemovals()` で制御スレッド上から排他的に削除 |

### コールバック直接削除の禁止

Timer callback (`DeferredVerifyTimerCallback`, `PersistentEnforceTimerCallback`) は `EnqueueRequest()` のみを実行し、ブロッキング操作やコンテキストの自己削除を行いません。

- **Deferred コンテキスト** (one-shot): 制御ループが `DispatchEnforcementRequest()` でリクエスト処理後に `delete` する
- **Persistent コンテキスト** (recurring): `Stop()` が `DeleteTimerQueueEx(INVALID_HANDLE_VALUE)` で全コールバック完了を待機した後に `delete` する

### pending removal キュー

プロセス終了コールバック `OnProcessExit` は OS スレッドプール上で実行されるため、直接 `RemoveTrackedProcess()` を呼ぶことはできません。

1. `OnProcessExit` → PID を `pendingRemovalPids_` に `push` + `hWakeupEvent_` をシグナル
2. Engine control loop が `WAIT_PROCESS_EXIT` で起床
3. `ProcessPendingRemovals()` が `CriticalSection` + `swap` パターンでキューを排他的に排出し、制御スレッド上で `RemoveTrackedProcess()` を実行

### EcoQoS ポリシー: 5 層防御

`PulseEnforceV6()` は以下の 5 層で EcoQoS を無効化します。

| 層 | API | 説明 |
|----|-----|------|
| 1 | `SetPriorityClass(PROCESS_MODE_BACKGROUND_END)` | Background Mode を解除 |
| 2 | `NtSetInformationProcess` (Win11) | 低レベル NT API で Power Throttling を OFF |
| 3 | `SetProcessInformation` (Win10 / フォールバック) | Win32 API で Power Throttling を OFF |
| 4 | `SetPriorityClass(HIGH_PRIORITY_CLASS)` | 優先度クラスを HIGH に設定し OS による自動 EcoQoS を防止 |
| 5 | `DisableThreadThrottling` (INTENSIVE 時のみ) | 全スレッドの Power Throttling を個別無効化 |

加えて、レジストリポリシー (`PowerThrottling` + `Image File Execution Options`) により、OS 再起動後も EcoQoS 除外が永続します。

### 3 フェーズ適応制御

| フェーズ | 動作 | 遷移条件 |
|---------|------|---------|
| **AGGRESSIVE** (0-3 秒) | 即時 enforce + 遅延検証 3 回 (200ms / 1s / 3s) | 3 回すべてクリーン → STABLE、違反 3 回以上 → PERSISTENT |
| **STABLE** | イベント駆動のみ (CPU ゼロ) | ETW スレッドイベント or Safety Net で違反検知 → AGGRESSIVE (違反 < 3) or PERSISTENT (違反 >= 3) |
| **PERSISTENT** | 5 秒間隔 enforce + ETW ブーストによる即時応答 | 60 秒クリーンで STABLE に復帰 |

### 安全性保証

- **Timer callback = enqueue only**: コールバック内でのブロッキング・`delete`・`RemoveTrackedProcess` を禁止
- **RemoveTrackedProcess = 制御スレッド専用**: OS スレッドプールからは `pendingRemovalPids_` 経由でのみアクセス
- **Stop() = 9 ステップバリア順序保証**: ETW 停止 → スレッド join → Timer Queue 破棄 → Wait ハンドル解除 → Job Object 解放 → レジストリクリーンアップ
- **UAF 防止**: `shared_ptr<TrackedProcess>` による参照カウントで、コールバック実行中のプロセスコンテキスト早期破棄を防止

---

## スレッドモデル

UnLeaf は固定数のスレッドで動作します。長時間稼働してもスレッド数は増加しません。

| コンポーネント | スレッド | 説明 |
|---------------|---------|------|
| **Service** | EngineControlThread (1) | WaitForMultipleObjects で全イベントを処理 |
| | ETW Consumer Thread (1) | OS のイベントトレーシングセッション |
| | IPC Server Thread (1) | Named Pipe 接続受付 |
| **Manager** | UI Thread (1) | Win32 メッセージループ |
| | Log Watcher Thread (1) | ログファイル差分監視 |

Timer Queue のコールバックは OS スレッドプール上で実行されますが、UnLeaf が作成するスレッドではありません。

---

## 24/7 常駐設計

UnLeaf は 24/7/365 常駐サービスとして設計されており、以下の原則に従います:

- **Idle is King**: イベントがなければ CPU を一切使用しない (`WaitForMultipleObjects(INFINITE)`)
- **固定リソース**: スレッド数・ハンドル数・メモリ使用量は起動後一定。プロセスの追加・削除を繰り返してもリソースが増加しない
- **RAII + 明示的解放**: 全てのタイマーコンテキスト・Wait コールバックコンテキストはライフサイクルが管理され、プロセス終了・タイマー再作成時に確実に解放される
- **ブロッキング解除**: `UnregisterWaitEx(INVALID_HANDLE_VALUE)` と `DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)` でコールバック完了を保証してからコンテキストを解放
- **IPC タイムアウト**: クライアント接続の ReadFile は Overlapped I/O + 5秒タイムアウトで実装。フリーズしたクライアントがサーバースレッドをブロックしない

---

## ライセンス

本リポジトリで公開されているソースコード（コアエンジン部分）は、MIT License の下で公開されています。

---

## 開発者向け情報

### ビルド手順

**前提**: Visual Studio 2022 以降 (MSVC)、CMake 3.20+

```cmd
# 設定
cmake -B build -G "Visual Studio 17 2022" -A x64

# Release ビルド
git clone https://github.com/itbizmonky/UnLeaf.git
cd UnLeaf
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

ビルドされた UnLeaf_Service.exe は、単独のWindowsサービスとしてシステムに登録し、動作させることが可能です。詳細な仕様とIPC通信インターフェースについては仕様書を参照してください。

### ソースコード構成

```
UnLeaf/
├── .github/
│   └── workflows/
│       └── build.yml            # GitHub Actions CI
├── CHANGELOG.md                 # 更新履歴
├── CMakeLists.txt               # OSS用動的ビルドスクリプト
├── LICENSE                      # MITライセンス
├── README.md / README_EN.md
├── docs/
│   └── Engine_Specification.md  # エンジンの詳細な技術仕様書
├── resources/
│   └── service.rc               # サービス用リソースファイル
├── src/
│   ├── common/                  # 共通ユーティリティ (ロガー、設定、レジストリ管理)
│   ├── engine/                  # エンジン決定ロジック (Win32 非依存・純粋 C++)
│   │   ├── engine_logic.*       # フェーズ遷移・EcoQoS 適用判定 (5 関数)
│   │   └── engine_policy.h      # タイミング定数集約 (EnginePolicy 構造体)
│   ├── service/                 # コアエンジン本体 (ETW 監視、サービス制御)
│   └── manager/                 # Manager UI (クローズドソース・ビルド対象外)
└── tests/                       # ユニットテスト (104 件 / 全 PASS)
```

---

## 更新履歴 (Changelog)

### v1.0.1 (2026-03-09)

**バグ修正・安全性強化**
- `DeleteTimerQueueTimer` を `trackedCs_` 取得前に移動し、ロック保持中の呼び出しを完全排除 (全5箇所)
- `DrawButton` の GDI オブジェクト復元処理を修正 (`SelectObject` で旧フォントを明示的に復元)
- `ToggleSubclassProc` に null チェックを追加

**アーキテクチャ改善**
- エンジン決定ロジックを `src/engine/engine_logic` に分離 (Win32 非依存の純粋 C++ モジュール)
- `EnginePolicy` 構造体を導入し、タイミング定数を `engine_core` から分離・一元管理
- ユニットテスト 72 件 → 104 件に拡充 (`test_engine_logic`: 32 件 + `test_engine_policy`: 2 件追加)

**リソース管理**
- `DrawToggleSwitch()` の GDI+ 初期化を RAII 化し、プロセス終了時に `GdiplusShutdown()` が確実に呼ばれるよう改善

**CI/CD**
- GitHub Actions CI を導入 (`push` / `pull_request` 時に自動ビルド + ctest)
- CI キャッシュパスを `build/_deps` に最適化（FetchContent 依存のみ対象、cache hit 率向上）

### v1.0.0 (2026-03-06)

初回リリース。