# UnLeaf Engine 開発者仕様書

> **対象バージョン**: v1.0.3 (C++ Native)
> **最終更新**: 2026-03-24
> **対象読者**: 本プロジェクトの保守・拡張を行う開発者
> **CI**: GitHub Actions による自動ビルド・ユニットテスト検証を push / pull_request 毎に実施。

---

## 目次

- [第1部: 概要設計 (Overview Design)](#第1部-概要設計-overview-design)
  - [1. システム概要](#1-システム概要)
  - [2. アーキテクチャ全体像](#2-アーキテクチャ全体像)
  - [3. EcoQoS 解除の全体処理フロー](#3-ecoqos-解除の全体処理フロー)
- [第2部: 基本設計 (Basic Design)](#第2部-基本設計-basic-design)
  - [4. EngineCore 基本設計](#4-enginecore-基本設計)
    - [4.5 EnginePolicy 構造体](#45-enginepolicy-構造体)
  - [5. 3フェーズ適応制御](#5-3フェーズ適応制御)
  - [6. EcoQoS 無効化 (PulseEnforceV6)](#6-ecoqos-無効化-pulseenforcev6)
  - [7. ProcessMonitor (ETW)](#7-processmonitor-etw)
  - [8. IPCServer](#8-ipcserver)
  - [9. 設定管理 (UnLeafConfig)](#9-設定管理-unleafconfig)
  - [10. RegistryPolicyManager](#10-registrypolicymanager)
  - [11. ログシステム](#11-ログシステム)
- [第3部: 詳細設計 (Detailed Design)](#第3部-詳細設計-detailed-design)
  - [12. EngineCore 詳細設計](#12-enginecore-詳細設計)
  - [13. メモリ管理・ライフタイム管理](#13-メモリ管理ライフタイム管理)
    - [13.5 engine_logic 純粋ロジックモジュール](#135-engine_logic-純粋ロジックモジュール)
  - [14. 同期・排他制御](#14-同期排他制御)
  - [15. エラー処理・自己修復](#15-エラー処理自己修復)
  - [16. Job Object 管理](#16-job-object-管理)
  - [17. セキュリティ](#17-セキュリティ)
- [付録](#付録)
  - [A. 定数一覧](#a-定数一覧)
  - [B. IPC コマンド一覧](#b-ipc-コマンド一覧)
  - [C. レジストリキー一覧](#c-レジストリキー一覧)
  - [D. クリティカルプロセス保護リスト](#d-クリティカルプロセス保護リスト)

---

# 第1部: 概要設計 (Overview Design)

## 1. システム概要

### 1.1 目的

UnLeaf は、Windows 11 の **EcoQoS (Efficiency Mode)** がユーザー指定のプロセスに自動適用されることを防止する Windows サービスである。EcoQoS は CPU のエネルギー効率を優先するため、ゲームやクリエイティブ系アプリケーションの性能を低下させる場合がある。UnLeaf はこの挙動を検知し、即座に解除することで、指定プロセスのパフォーマンスを維持する。

### 1.2 解決する課題

| 課題 | UnLeaf の対応 |
|------|-------------|
| OS がバックグラウンドプロセスに EcoQoS を自動適用する | ETW イベント駆動でリアルタイム検知・即時解除 |
| スレッド生成時に EcoQoS が再適用される | ETW Thread Start イベントで即座に再解除 |
| SetProcessInformation だけでは OS に上書きされる | 5層防御 (NT API + Win32 API + Priority + Thread + Registry) |
| 監視ポーリングによる CPU 負荷 | イベント駆動アーキテクチャで待機時 CPU 使用率 ≈ 0% |
| プロセス起動検知の遅延 | ETW コールバックにより ~1ms で検知 |

### 1.3 動作環境

| 項目 | 要件 |
|------|------|
| OS | Windows 10 / Windows 11 (Build 22000+) |
| 権限 | SYSTEM アカウント (Windows サービスとして動作) |
| 依存 | ntdll.dll, advapi32.lib, tdh.lib |
| アーキテクチャ | x64 |
| ランタイム | C++17, MSVC (Visual Studio 2022 以降) |

### 1.4 用語定義

| 用語 | 説明 |
|------|------|
| **EcoQoS** | Windows 11 で導入された電力効率化機能。プロセスの CPU パフォーマンスを制限する |
| **Power Throttling** | EcoQoS の内部名。`PROCESS_POWER_THROTTLING_STATE` 構造体で制御される |
| **ETW** | Event Tracing for Windows。カーネルレベルのイベント通知メカニズム |
| **WFMO** | `WaitForMultipleObjects`。複数のカーネルオブジェクトを同時に待機する Win32 API |
| **フェーズ** | AGGRESSIVE / STABLE / PERSISTENT の 3 状態。プロセスごとの監視強度を表す |
| **Pulse Enforce** | EcoQoS 状態を問わず強制的に OFF に設定する操作 |
| **Safety Net** | 10 秒周期の保険チェック。ETW イベント漏れや OS クイークへの対策 |
| **IFEO** | Image File Execution Options。レジストリベースの exe 起動時設定 |
| **Job Object** | Windows カーネルオブジェクト。プロセスグループの管理に使用 |
| **TrackedProcess** | UnLeaf が管理対象として追跡しているプロセスの情報構造体 |

---

## 2. アーキテクチャ全体像

### 2.1 システム構成図

```
図1: システム構成図

  ┌─────────────────┐         ┌─────────────────────────────────────────────┐
  │  Manager.exe    │         │  UnLeaf Service (unleaf_service.exe)        │
  │  (GUI/CLI)      │         │                                             │
  │                 │ Named   │  ┌─────────────┐  ┌──────────────────────┐  │
  │  ユーザー操作   │◄═Pipe═══►│  IPCServer    │  │  EngineCore          │  │
  │  ターゲット設定 │         │  │  (認可付き)  │  │  (Singleton)         │  │
  │  ログ閲覧      │         │  └─────────────┘  │                      │  │
  │  ヘルスチェック │         │                    │  ┌────────────────┐  │  │
  └─────────────────┘         │                    │  │EngineControl   │  │  │
                              │                    │  │Loop (WFMO)     │  │  │
                              │                    │  └────────────────┘  │  │
                              │                    │         ▲            │  │
                              │                    │         │ Enqueue    │  │
                              │  ┌─────────────┐   │  ┌──────┴────────┐  │  │
                              │  │ Process     │───►│  │ Enforcement  │  │  │
                              │  │ Monitor     │   │  │ Queue        │  │  │
                              │  │ (ETW)       │   │  └──────────────┘  │  │
                              │  └──────┬──────┘   │                    │  │
                              │         │          └──────────────────────┘  │
                              │         │                                    │
                              │  ┌──────▼──────┐  ┌─────────────────────┐   │
                              │  │ Windows     │  │ Registry Policy     │   │
                              │  │ Kernel      │  │ Manager             │   │
                              │  │ (ETW prov.) │  │ (PowerThrottling    │   │
                              │  └─────────────┘  │  + IFEO)            │   │
                              │                   └─────────────────────┘   │
                              └─────────────────────────────────────────────┘
```

### 2.2 コンポーネント一覧

| コンポーネント | ソースファイル | 責務 |
|---------------|---------------|------|
| **EngineCore** | `engine_core.h/cpp` | エンジン全体の制御。フェーズ管理、EcoQoS 解除、プロセス追跡 |
| **engine_logic** | `src/engine/engine_logic.h/cpp` | 純粋 C++ 決定ロジック (Win32 依存なし)。IsTargetProcess / IsCacheValid / ShouldExitPersistent / NextPhaseOnViolation / DeferredVerifyDelayMs |
| **EnginePolicy** | `src/engine/engine_policy.h` | エンジン動作パラメータ構造体。EngineCore が `policy_` メンバとして保持し、engine_logic 関数へ参照渡しする |
| **ProcessMonitor** | `process_monitor.h/cpp` | ETW セッション管理。プロセス起動・スレッド生成イベントの受信 |
| **IPCServer** | `ipc_server.h/cpp` | Named Pipe サーバー。Manager との通信、認可処理 |
| **UnLeafConfig** | `config.h/cpp` | INI 設定ファイルの読み書き、変更検知 |
| **RegistryPolicyManager** | `registry_manager.h/cpp` | レジストリポリシーの適用・クリーンアップ・マニフェスト永続化 |
| **LightweightLogger** | `logger.h/cpp` | スレッドセーフなファイルロガー (100KB ローテーション) |
| **ScopedHandle 群** | `scoped_handle.h` | RAII ハンドル管理 |
| **Security** | `security.h` | Named Pipe DACL、トークン検証 |
| **Types** | `types.h` | 共通型定義、定数、バリデーション関数 |

### 2.3 スレッドモデル

```
┌──────────────────────────────────────────────────────────────────┐
│                        スレッド構成                              │
├──────────────────────┬───────────────────────────────────────────┤
│ メインスレッド       │ サービス登録・開始・停止制御              │
│ EngineControlThread  │ WFMO ループ、キュー処理、保守タスク      │
│ ETW ConsumerThread   │ ProcessTrace() ブロッキング、イベント受信│
│ IPC ServerThread     │ Named Pipe 接続待機・コマンド処理        │
│ OS Thread Pool       │ Timer Queue コールバック、Wait コールバック│
└──────────────────────┴───────────────────────────────────────────┘
```

UnLeaf のコアは **3 スレッド + OS スレッドプール** で構成される。

**UnLeaf が作成するスレッド**:
- **`engineControlThread_`**: `EngineControlLoop()` を実行する単一制御スレッド。WFMO で 5 つのハンドルを待機し、全ての状態変更をこのスレッド上でシリアルに処理する
- **ETW ConsumerThread**: `ProcessTrace()` のブロッキングコールを実行する。OS がコールバック (`OnProcessStart` / `OnThreadStart`) を呼び出し、`EnqueueRequest()` 経由で engineControlThread_ にリクエストを送る
- **IPC ServerThread**: Named Pipe の接続待機とコマンド処理を行う

**OS が管理するスレッド** (UnLeaf は生成しない):
- **タイマーキュースレッドプール**: `CreateTimerQueueTimer` により OS スレッドプール上でデファードコールバック (`DeferredVerifyTimerCallback` / `PersistentEnforceTimerCallback`) が実行される。コールバックは `EnqueueRequest()` のみ実行し、ブロッキング操作やコンテキスト自己削除は行わない
- **WaitCallback スレッド**: `RegisterWaitForSingleObject` によるプロセス終了通知 (`OnProcessExit`)。PID を `pendingRemovalPids_` に push し `hWakeupEvent_` をシグナルして engineControlThread_ に処理を委譲する

### 2.4 データフロー

```
ETW Kernel Event ──► ProcessMonitor ──► OnProcessStart() / OnThreadStart()
                                              │
                                              ▼
                                        EnqueueRequest()
                                              │
                                              ▼
                               enforcementQueue_ + SetEvent()
                                              │
                                              ▼
                                   EngineControlLoop (WFMO)
                                              │
                                              ▼
                                   DispatchEnforcementRequest()
                                              │
                                     ┌────────┴────────┐
                                     ▼                  ▼
                              PulseEnforceV6()   Phase Transition
                                     │
                              ┌──────┴──────┐
                              ▼              ▼
                    NtSetInformation   SetProcessInformation
                    Process (Win11)   (Win10 fallback)
```

### 2.5 設計原則

| 原則 | 説明 |
|------|------|
| **イベント駆動** | ポーリングを行わない。ETW イベント、タイマーコールバック、WFMO による完全イベント駆動 |
| **単一制御スレッド** | 全ての状態変更は EngineControlThread で実行。ETW コールバックはキュー経由で委譲 |
| **多層防御** | EcoQoS 解除に 5 層の手段を適用し、いずれかの失敗に耐える |
| **自己修復** | ハンドル無効化やアクセス拒否をエラーレベルに応じて自動回復 |
| **縮退運転** | ETW 障害時は DEGRADED_ETW モードで Toolhelp32 フォールバックに自動切替 |
| **冪等性** | レジストリポリシーの適用・削除は何度呼んでも同じ結果 |
| **ゼロトラスト** | PulseEnforce は現在の EcoQoS 状態を前提とせず、常に OFF を強制する |

---

## 3. EcoQoS 解除の全体処理フロー

### 3.1 プロセス検出 → STABLE 到達シーケンス

```
図7: プロセス検出 → STABLE 到達シーケンス

  ┌─────────────────────┐
  │ ETW: Process Start  │   Microsoft-Windows-Kernel-Process
  │ (Event ID: 1)       │   ~1ms で検知
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ OnProcessStart()    │   ターゲット名照合 / 親PID追跡
  │  - IsTargetName()   │   クリティカルプロセスフィルタ
  │  - IsTrackedParent()│
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ ApplyOptimization() │   1. OpenProcess (0x1200)
  │                     │   2. Registry Policy 適用 (初回のみ)
  │                     │   3. PulseEnforceV6 (5層防御)
  │                     │   4. Job Object 作成/割当
  │                     │   5. RegisterWaitForSingleObject
  │                     │   6. TrackedProcess 登録
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ Phase: AGGRESSIVE   │   Phase 開始
  │                     │   ScheduleDeferredVerification(step=1)
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ Deferred Verify #1  │   200ms 後 (Timer Queue コールバック)
  │ IsEcoQoSEnabled?    │
  │   OFF → step 2 へ   │
  │   ON  → re-enforce  │
  └──────────┬──────────┘
             ▼ (clean)
  ┌─────────────────────┐
  │ Deferred Verify #2  │   1000ms 後
  │ IsEcoQoSEnabled?    │
  │   OFF → step 3 へ   │
  │   ON  → re-enforce  │
  └──────────┬──────────┘
             ▼ (clean)
  ┌─────────────────────┐
  │ Deferred Verify #3  │   3000ms 後 (最終検証)
  │ IsEcoQoSEnabled?    │
  │   OFF → STABLE へ   │
  │   ON  → violation++ │
  └──────────┬──────────┘
             ▼ (clean)
  ┌─────────────────────┐
  │ Phase: STABLE       │   イベント駆動のみ
  │ (定常状態)          │   アクティブポーリングなし
  └─────────────────────┘
```

### 3.2 違反検知 → 復旧フロー

```
図8: 違反検知 → 復旧フロー

  ┌─────────────────────┐
  │ ETW: Thread Start   │   スレッド生成 = EcoQoS 再適用トリガー
  │ (Event ID: 3)       │
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ OnThreadStart()     │   tracked PID フィルタ (O(1) lookup)
  │ phase == STABLE?    │
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ EnqueueRequest      │   ETW_THREAD_START
  │ → EngineControlLoop │
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ IsEcoQoSEnabled?    │   NtQuery / GetProcessInformation
  │   OFF → no action   │
  │   ON  → violation   │
  └──────────┬──────────┘
             ▼ (violation)
  ┌─────────────────────┐
  │ PulseEnforceV6()    │   5層防御で即座に解除
  │ violationCount++    │
  └──────────┬──────────┘
             │
             ├─── violationCount < 3 ──► AGGRESSIVE (再検証シーケンス)
             │
             └─── violationCount >= 3 ──► PERSISTENT (5s 周期エンフォース)
```

### 3.3 PERSISTENT → STABLE 復帰フロー

```
  ┌─────────────────────┐
  │ Phase: PERSISTENT   │   5 秒周期タイマーで IsEcoQoSEnabled チェック
  │                     │   + ETW Thread Start で即時ブースト (1s rate limit)
  └──────────┬──────────┘
             │
             │  60 秒間 violation なし
             │  (lastViolationTime から計算)
             │
             ▼
  ┌─────────────────────┐
  │ Phase: STABLE       │   タイマーキャンセル
  │ (定常状態に復帰)    │   イベント駆動のみに戻る
  └─────────────────────┘
```

---

# 第2部: 基本設計 (Basic Design)

## 4. EngineCore 基本設計

### 4.1 ライフサイクル

```
  ┌───────────┐     Initialize()     ┌──────────┐     Start()     ┌─────────┐
  │ 未初期化  │ ──────────────────► │ 初期化済 │ ────────────► │ 実行中  │
  └───────────┘                      └──────────┘                 └────┬────┘
                                                                       │
                                                                  Stop()
                                                                       │
                                                                  ┌────▼────┐
                                                                  │ 停止済  │
                                                                  └─────────┘
```

- **Initialize()**: 設定読み込み、ハンドル生成、NT API 解決、Windows バージョン検出
- **Start()**: ETW 開始、初期スキャン、Safety Net タイマー設定、EngineControlThread 起動
- **Stop()**: 9 ステップの順序付きシャットダウン (§12.4 参照)

EngineCore は **Singleton** パターンで実装され、`EngineCore::Instance()` でアクセスする。

### 4.2 WFMO 制御ループ構成

```
図2: WFMO 制御ループ

  EngineControlLoop()
  │
  │  waitHandles[5] = {
  │    [0] stopEvent_              ← サービス停止シグナル (Manual Reset)
  │    [1] configChangeHandle_     ← FindFirstChangeNotification
  │    [2] safetyNetTimer_         ← Waitable Timer (10s 周期)
  │    [3] enforcementRequestEvent_← Auto-Reset (キュー非空時)
  │    [4] hWakeupEvent_           ← Auto-Reset (プロセス終了通知)
  │  };
  │
  │  while (!stopRequested_) {
  │    DWORD result = WaitForMultipleObjects(5, handles, FALSE, INFINITE)
  │    │
  │    ├── WAIT_STOP (0)          → break (ループ終了)
  │    ├── WAIT_CONFIG_CHANGE (1) → configChangePending_ = true
  │    │                            FindNextChangeNotification()
  │    ├── WAIT_SAFETY_NET (2)    → HandleSafetyNetCheck()
  │    ├── WAIT_ENFORCEMENT (3)   → ProcessEnforcementQueue()
  │    └── WAIT_PROCESS_EXIT (4)  → ProcessPendingRemovals()
  │
  │    // Debounced config reload
  │    if (configChangePending_ && debounce elapsed)
  │      → HandleConfigChange()
  │
  │    // Piggybacked maintenance
  │    PerformPeriodicMaintenance(now)
  │  }
```

WaitForMultipleObjects は `INFINITE` タイムアウトで呼ばれる。CPU を消費するポーリングは一切行わない。待機中のスレッドは OS スケジューラによって休眠状態となる。

### 4.3 キュー設計

EngineCore は 2 つのスレッドセーフキューを持つ。

#### 4.3.1 enforcementQueue_ (エンフォースメントリクエストキュー)

| 項目 | 内容 |
|------|------|
| 型 | `std::queue<EnforcementRequest>` |
| 保護 | `queueCs_` (CriticalSection) |
| 生産者 | ETW コールバック、Timer Queue コールバック |
| 消費者 | EngineControlThread (`ProcessEnforcementQueue`) |
| 通知 | `enforcementRequestEvent_` (Auto-Reset Event) |

```
図6: Enforcement Queue フロー

  OS Thread Pool / ETW Thread
        │
        ▼
  EnqueueRequest()
        │  CSLockGuard(queueCs_)
        │  push(req)
        │  if (wasEmpty) SetEvent(enforcementRequestEvent_)
        ▼
  EngineControlThread (WFMO wakeup)
        │
        ▼
  ProcessEnforcementQueue()
        │  CSLockGuard(queueCs_)
        │  swap(pending, enforcementQueue_)  ← 全件取得
        │
        ▼
  while (!pending.empty())
        │  DispatchEnforcementRequest(req)
        └──► Phase ハンドラへ
```

- `EnqueueRequest()` は **キューが空だった場合のみ** `SetEvent()` を呼ぶ。これにより不要なイベントシグナルを抑制する。
- `ProcessEnforcementQueue()` はキューの **swap** で全件を取得し、ロック保持時間を最小化する。

#### 4.3.2 pendingRemovalPids_ (プロセス終了通知キュー)

| 項目 | 内容 |
|------|------|
| 型 | `std::queue<DWORD>` |
| 保護 | `pendingRemovalCs_` (CriticalSection) |
| 生産者 | OS Thread Pool (`OnProcessExit` コールバック) |
| 消費者 | EngineControlThread (`ProcessPendingRemovals`) |
| 通知 | `hWakeupEvent_` (Auto-Reset Event) |

`OnProcessExit` は `RegisterWaitForSingleObject` のコールバックとして OS スレッドプール上で呼ばれる。直接 `RemoveTrackedProcess()` を呼ぶとロック競合が発生するため、PID をキューに入れて EngineControlThread に処理を委譲する。

### 4.4 タイミング定数

| 定数名 | 値 | 用途 |
|--------|-----|------|
| `AGGRESSIVE_DURATION` | 3,000 ms | AGGRESSIVE フェーズ全体の持続時間 |
| `DEFERRED_VERIFY_1` | 200 ms | 1 回目の遅延検証タイミング |
| `DEFERRED_VERIFY_2` | 1,000 ms | 2 回目の遅延検証タイミング |
| `DEFERRED_VERIFY_FINAL` | 3,000 ms | 最終検証タイミング |
| `PERSISTENT_ENFORCE_INTERVAL` | 5,000 ms | PERSISTENT フェーズのエンフォース周期 |
| `PERSISTENT_CLEAN_THRESHOLD` | 60,000 ms | PERSISTENT → STABLE 遷移条件 (違反なし期間) |
| `ETW_BOOST_RATE_LIMIT` | 1,000 ms | PERSISTENT での ETW ブースト レートリミット |
| `SAFETY_NET_INTERVAL` | 10,000 ms | Safety Net チェック周期 |
| `VIOLATION_THRESHOLD` | 3 回 | PERSISTENT 遷移に必要な違反回数 |
| `STATS_LOG_INTERVAL` | 60,000 ms | 統計ログ出力周期 |
| `JOB_QUERY_INTERVAL` | 5,000 ms | Job Object PID リフレッシュ周期 |
| `ETW_HEALTH_CHECK_INTERVAL` | 30,000 ms | ETW ヘルスチェック周期 |
| `DEGRADED_SCAN_INTERVAL` | 30,000 ms | DEGRADED モードフォールバックスキャン周期 |
| `CONFIG_DEBOUNCE_MS` | 2,000 ms | 設定変更デバウンス時間 |
| `ERROR_LOG_SUPPRESS_MS` | 60,000 ms | エラーログ抑制ウィンドウ (同一 PID × エラーコード) |
| `ETW_STABLE_RATE_LIMIT` | 200 ms | STABLE フェーズでの ETW スレッドイベント レートリミット |
| `ECOQOS_CACHE_DURATION` | 100 ms | IsEcoQoSEnabledCached マイクロキャッシュ TTL |
| `LIVENESS_CHECK_INTERVAL` | 60,000 ms | ゾンビ TrackedProcess 検出間隔 |

### 4.5 EnginePolicy 構造体

§8.42 で導入した `engine_logic::EnginePolicy` 構造体 (`src/engine/engine_policy.h`) は、エンジンの動作パラメータを一元管理する。`EngineCore` は `policy_` メンバ (インライン初期化) として保持し、純粋ロジック関数へ `const` 参照渡しする。

| フィールド | 型 | デフォルト値 | 対応する定数 |
|-----------|-----|------------|------------|
| `violationThreshold` | `uint32_t` | 3 | `VIOLATION_THRESHOLD` |
| `cacheDurationMs` | `uint64_t` | 100 ms | `ECOQOS_CACHE_DURATION` |
| `verifyDelay1Ms` | `uint32_t` | 200 ms | `DEFERRED_VERIFY_1` |
| `verifyDelay2Ms` | `uint32_t` | 1,000 ms | `DEFERRED_VERIFY_2` |
| `verifyDelayFinalMs` | `uint32_t` | 3,000 ms | `DEFERRED_VERIFY_FINAL` |

`policy_` は `engine_core.h` 内でインライン初期化されるため、`Initialize()` 内での明示的な構築処理は不要である。

---

## 5. 3フェーズ適応制御

### 5.1 状態遷移図

```
図3: フェーズ状態遷移図

                    プロセス検出
                        │
                        ▼
              ┌──────────────────┐
              │    AGGRESSIVE    │
              │  (起動時適応)    │
              │                  │
              │ One-shot SET +   │
              │ 遅延検証 3回     │
              │ (200ms/1s/3s)    │
              └────────┬─────────┘
                       │
              3回検証パス (EcoQoS OFF)
                       │
                       ▼
              ┌──────────────────┐
  violation   │     STABLE       │   violation
  < 3 回  ┌──│  (定常状態)      │──┐  >= 3 回
          │  │                  │  │
          │  │ イベント駆動のみ │  │
          │  │ ポーリングなし   │  │
          │  └──────────────────┘  │
          │                        │
          ▼                        ▼
  ┌──────────────┐      ┌──────────────────┐
  │  AGGRESSIVE  │      │   PERSISTENT     │
  │  (再検証)    │      │  (頑固な EcoQoS) │
  └──────────────┘      │                  │
                        │ 5s 周期 enforce  │
                        │ + ETW boost      │
                        └────────┬─────────┘
                                 │
                        60s clean (violation なし)
                                 │
                                 ▼
                        ┌──────────────────┐
                        │     STABLE       │
                        └──────────────────┘
```

### 5.2 各フェーズの動作

#### AGGRESSIVE フェーズ

- **トリガー**: プロセス検出時、または STABLE で violation 検知時 (< 3 回)
- **動作**: 初回 PulseEnforceV6 実行後、Timer Queue による遅延検証を 3 段階で実施
  - Step 1: 200ms 後に `IsEcoQoSEnabled` チェック
  - Step 2: Step 1 完了から 800ms 後にチェック (起動から累計 ~1,000ms)
  - Step 3: Step 2 完了から 2,000ms 後にチェック (起動から累計 ~3,000ms、最終検証)
- **タイマー間隔の計算** (`ScheduleDeferredVerification`):
  ```
  step 1: delayMs = DEFERRED_VERIFY_1                          = 200ms
  step 2: delayMs = DEFERRED_VERIFY_2 - DEFERRED_VERIFY_1      = 800ms (相対)
  step 3: delayMs = DEFERRED_VERIFY_FINAL - DEFERRED_VERIFY_2  = 2000ms (相対)
  ```
  各 step は前の step のコールバック完了後に次の step をスケジュールするため、相対的な遅延となる。
- **遷移条件**:
  - 全ステップ clean → **STABLE**
  - 検証中に EcoQoS ON → `violationCount++`, PulseEnforceV6 再実行, 検証シーケンスリセット (step 1 から再開)
  - `violationCount >= 3` → **PERSISTENT** (全タイマーキャンセル後、persistent タイマー開始)

#### STABLE フェーズ

- **トリガー**: AGGRESSIVE の 3 回検証パス、または PERSISTENT の 60s clean
- **動作**: アクティブなポーリングやタイマーなし。以下のイベントでのみ処理:
  - ETW Thread Start → `IsEcoQoSEnabledCached` チェック (200ms レートリミット: `ETW_STABLE_RATE_LIMIT`)
  - Safety Net (10s) → `IsEcoQoSEnabled` チェック
- **遷移条件**:
  - EcoQoS violation 検知 → `violationCount++`
  - `violationCount < 3` → **AGGRESSIVE** (再検証)
  - `violationCount >= 3` → **PERSISTENT**

#### PERSISTENT フェーズ

- **トリガー**: `violationCount >= VIOLATION_THRESHOLD (3)`
- **動作**:
  - `CreateTimerQueueTimer` による 5 秒周期の recurring タイマー
  - 毎回 `IsEcoQoSEnabled` → ON なら `PulseEnforceV6`
  - ETW Thread Start でも即時ブースト (1s rate limit: `lastEtwEnforceTime`)
- **遷移条件**:
  - 60 秒間 violation なし (`PERSISTENT_CLEAN_THRESHOLD`) → **STABLE**

### 5.3 遷移条件まとめ

| 遷移元 | 遷移先 | 条件 |
|--------|--------|------|
| AGGRESSIVE | STABLE | 3 段階の遅延検証すべて clean |
| AGGRESSIVE | PERSISTENT | violationCount >= 3 |
| STABLE | AGGRESSIVE | violation 検知 (count < 3) |
| STABLE | PERSISTENT | violation 検知 (count >= 3) |
| PERSISTENT | STABLE | 60 秒間 violation なし |

---

## 6. EcoQoS 無効化 (PulseEnforceV6)

### 6.1 5層防御構造

PulseEnforceV6 は「ゼロトラスト」原則に基づく。現在の EcoQoS 状態を前提とせず、常に OFF を強制する。

> **Note**: コード内コメントでは「Layer 1 = レジストリポリシー」と記載されているが、レジストリポリシーは `ApplyOptimization()` で初回のみ適用されるため、PulseEnforceV6 関数内の処理は Step 1-5 として記載する。

```
図4: EcoQoS 解除 5層防御フロー

  PulseEnforceV6(hProcess, pid, isIntensive)
  │
  │  Step 1: Background Mode Exit (無条件)
  │  ├── SetPriorityClass(hProcess, PROCESS_MODE_BACKGROUND_END)
  │  │   OS がバックグラウンドモード化している場合の即時解除
  │  │   戻り値は無視 (バックグラウンドでない場合も副作用なし)
  │
  │  ControlMask 計算:
  │  ├── Win11: EXECUTION_SPEED(0x1) | IGNORE_TIMER(0x4) = 0x5
  │  └── Win10: EXECUTION_SPEED(0x1) のみ = 0x1
  │        (IGNORE_TIMER は Win10 で ERROR_INVALID_PARAMETER を引き起こすため除外)
  │
  │  Step 2: NtSetInformationProcess (Windows 11+ のみ)
  │  ├── 条件: winVersion_.isWindows11OrLater && ntApiAvailable_
  │  ├── ntdll!NtSetInformationProcess(hProcess, 77, &state, sizeof(state))
  │  │   UnleafThrottleState {
  │  │     Version = 1,
  │  │     ControlMask = 0x5 (EXECUTION_SPEED | IGNORE_TIMER),
  │  │     StateMask = 0 (Force OFF)
  │  │   }
  │  │
  │  │   STATUS_SUCCESS → ntApiSuccessCount_++, ecoQoSSuccess=true, Step 3 スキップ
  │  │   失敗 → ntApiFailCount_++, Step 3 へフォールバック
  │
  │  Step 3: SetProcessInformation (Win10 primary / Win11 fallback)
  │  ├── 条件: ecoQoSSuccess == false
  │  ├── SetProcessInformation(hProcess, ProcessPowerThrottling, &state, sizeof(state))
  │  │   StateMask = 0 (Force OFF)
  │  │   ControlMask = バージョン依存 (上記計算値)
  │
  │  Step 4: Priority Class (無条件)
  │  ├── SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS)
  │  │   EcoQoS 制御の成否に関わらず実行
  │  │   OS はプロセス優先度が HIGH の場合、自動 EcoQoS 適用を抑制する
  │
  │  Step 5: Thread Throttling (isIntensive == true のみ)
  │  └── DisableThreadThrottling(pid, aggressive=true)
  │       │
  │       ├── CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
  │       │
  │       └── 各スレッド (ownerPid == pid):
  │           ├── OpenThread(SET_INFORMATION | QUERY_INFORMATION)
  │           ├── SetThreadInformation(ThreadPowerThrottling)
  │           │   UnleafThreadThrottleState { Version=1, ControlMask=0x1, StateMask=0 }
  │           ├── GetThreadPriority(hThread)
  │           │   aggressive=true:  < ABOVE_NORMAL → SetThreadPriority(ABOVE_NORMAL)
  │           │   aggressive=false: IDLE/LOWEST/BELOW_NORMAL → SetThreadPriority(ABOVE_NORMAL)
  │           └── CloseHandle(hThread)
  │
  │  Error Handling:
  │  └── !ecoQoSSuccess → HandleEnforceError(hProcess, pid, GetLastError())
```

### 6.1.1 PulseEnforce (レガシーフォールバック)

`PulseEnforce()` は PulseEnforceV6 の簡易版であり、NtSetInformationProcess を使用しない:

- Step 1: Background mode exit
- Step 2: SetProcessInformation (ControlMask = 0x5 固定、バージョン分岐なし)
- Step 3: SetPriorityClass(HIGH_PRIORITY_CLASS)
- Step 4: DisableThreadThrottling(pid, **aggressive=false** ← 保守的モード)

現在のコードでは PulseEnforceV6 がすべてのパスで使用されているため、PulseEnforce はデッドコードとなっている。

### 6.2 IsEcoQoSEnabled

EcoQoS の現在状態を照会する。PulseEnforceV6 の **前** に呼び出して violation を判定する。

```
IsEcoQoSEnabled(hProcess)
  │
  ├── NtQueryInformationProcess (利用可能な場合)
  │   ProcessInformationClass = 77
  │   → (StateMask & EXECUTION_SPEED) != 0 → true
  │
  └── GetProcessInformation (フォールバック)
      ProcessPowerThrottling
      → (StateMask & EXECUTION_SPEED) != 0 → true

  判定不能 → false (EcoQoS OFF と見なす)
```

### 6.2.1 IsEcoQoSEnabledCached (マイクロキャッシュ)

スレッドバースト時の NtQueryInformationProcess 呼び出し抑制のため、100ms TTL のマイクロキャッシュを使用する。

```
IsEcoQoSEnabledCached(tp, now)
  ├── ecoQosCached && (now - ecoQosCacheTime < 100ms)
  │   → キャッシュ値を返す
  └── キャッシュミス → IsEcoQoSEnabled() → キャッシュ更新
```

STABLE / PERSISTENT での ETW_THREAD_START 処理で使用される。enforcement 後はキャッシュを無効化する (`ecoQosCached = false`)。

### 6.3 Windows バージョン分岐

| 機能 | Windows 11 (Build >= 22000) | Windows 10 |
|------|---------------------------|------------|
| NtSetInformationProcess | 使用 (Layer 2) | 不使用 |
| IGNORE_TIMER フラグ (0x4) | ControlMask に含む | 含まない (エラー回避) |
| SetProcessInformation | フォールバック (Layer 3) | プライマリ |
| NtQueryInformationProcess | IsEcoQoSEnabled で優先使用 | フォールバックのみ |

バージョン検出は `Initialize()` 時に `RtlGetVersion` で行う。`winVersion_.isWindows11OrLater` で Build 22000 以上を判定する。

起動ログのバージョン表示名はハイブリッド方式で決定する:

| 条件 | 表示名 |
|------|--------|
| major == 10, build < 22000 | `Windows 10` |
| major == 10, build >= 22000 (`isWindows11OrLater`) | `Windows 11` |
| major >= 11 | `Windows {major}.{minor}` (将来の Windows 12+ への自動対応) |

---

## 7. ProcessMonitor (ETW)

### 7.1 ETW セッション構成

| 項目 | 値 |
|------|-----|
| プロバイダ | Microsoft-Windows-Kernel-Process |
| GUID | `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}` |
| キーワード | `0x30` (PROCESS `0x10` \| THREAD `0x20`) |
| レベル | `TRACE_LEVEL_INFORMATION` |
| モード | `EVENT_TRACE_REAL_TIME_MODE` |
| クロック | QPC (`ClientContext = 1`) |
| セッション名 | `UnLeafProcessMonitor_<PID>` |
| バッファサイズ | `64 KB` (v1.0.2 で明示化) |
| 最小バッファ数 | `4` |
| 最大バッファ数 | `32` |
| フラッシュタイマー | `0` (リアルタイムコンシューマーモード) |

### 7.2 イベント処理

| Event ID | イベント名 | 処理 |
|----------|-----------|------|
| 1 | Process Start | `ParseProcessStartEvent` → PID, ParentPID, ImageName 抽出 → `processCallback_` |
| 2 | Process Stop | 受信するが処理しない (Wait コールバックで検知) |
| 3 | Thread Start | `pEvent->EventHeader.ProcessId` → `threadCallback_` |

- Process Start イベントのパースは **TDH (Trace Data Helper)** を使用する。OS バージョンによりイベント構造が異なるため、静的パースではなく TDH による動的パースを採用。
- ImageName は Unicode / ANSI の両方に対応し、フルパスからファイル名のみを抽出する。

### 7.3 ヘルスチェック

`IsHealthy()` は以下の条件をすべて満たす場合に `true` を返す:

1. `running_` == true
2. `sessionHealthy_` == true (OpenTrace / ProcessTrace が正常)
3. イベント枯渇なし: 最後のイベントから 60 秒以内 (ただし `eventCount_ > 0` の場合のみ)

EngineCore は 30 秒ごとに `IsHealthy()` を呼び出し、不健全な場合は ETW セッションの再起動を試みる。再起動に失敗した場合、`DEGRADED_ETW` モードに遷移する。

> **ETW 再起動時のイベント空白について**: ETW セッションの `Stop()` → `Start()` 中に極短時間のイベント空白が生じる可能性があるが、実害なし (確認済み)。Safety Net (10 秒間隔) が常時稼働しており、空白中に EcoQoS が再適用されても次回 Safety Net チェックで検出・解除される。ETW ヘルスチェック (30 秒インターバル) + 自動再起動機能の組み合わせで対処している。

### 7.4 静的インスタンスポインタ

ETW コールバックは C API であり、`this` ポインタを受け取れない。`ProcessMonitor::instance_` 静的メンバで唯一のインスタンスへのルーティングを行う。`Start()` で設定し、`Stop()` で `nullptr` にクリアする。

### 7.5 ETW 信頼性強化 (v1.0.2)

#### Zombie Session Cleanup

`Start()` の `StartTraceW` 呼び出し前に、同名セッションが残留していないかを確認し、存在する場合は `ControlTrace(EVENT_TRACE_CONTROL_STOP)` で停止してから開始する。

```cpp
// セッション開始前のゾンビセッション除去
TRACEHANDLE hExisting = 0;
if (QueryTrace(&hExisting, sessionName) == ERROR_SUCCESS) {
    ControlTrace(hExisting, sessionName, props, EVENT_TRACE_CONTROL_STOP);
}
```

**目的**: 前回の異常終了やクラッシュで同名セッションが残存した場合、`StartTraceW` が `ERROR_ALREADY_EXISTS` を返して ETW が起動できなくなる問題を防止する。

#### Lost Event Detection

ETW コールバック内で `EVENT_TRACE_TYPE_LOST_EVENT` opcode を監視する。バッファオーバーフロー等でイベントがドロップされた場合、`LOG_ALERT` を発行し累積カウント (`lostEventCount_`) をインクリメントする。

```cpp
if (pEvent->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_LOST_EVENT) {
    ++lostEventCount_;
    LOG_ALERT(L"[ETW] Lost event detected. Cumulative: " + std::to_wstring(lostEventCount_));
    return;
}
```

**目的**: ログにバッファオーバーフローの兆候を記録し、Safety Net や ETW 設定チューニングの参考情報を提供する。

---

## 8. IPCServer

### 8.1 Named Pipe 構成

| 項目 | 値 |
|------|-----|
| パイプ名 | `\\.\pipe\UnLeafServicePipe` |
| アクセスモード | `PIPE_ACCESS_DUPLEX \| FILE_FLAG_OVERLAPPED` |
| パイプモード | `PIPE_TYPE_MESSAGE \| PIPE_READMODE_MESSAGE` |
| インスタンス数 | `PIPE_UNLIMITED_INSTANCES` |
| バッファサイズ | 65,536 バイト (入出力各) |
| セキュリティ | DACL: SYSTEM + Administrators のみ |
| I/O タイムアウト | 5,000 ms (クライアントごと) |

### 8.2 認可モデル

```
  クライアント接続
       │
       ▼
  コマンド受信 (IPCMessage)
       │
       ▼
  GetCommandPermission(cmd) → PUBLIC / ADMIN / SYSTEM_ONLY
       │
       ├── PUBLIC  → 認可スキップ → ProcessCommand()
       │
       ├── ADMIN   → ImpersonateNamedPipeClient()
       │              OpenThreadToken()
       │              IsTokenAdmin() → true → ProcessCommand()
       │                             → false → RESP_ERROR_ACCESS_DENIED
       │
       └── SYSTEM_ONLY → ImpersonateNamedPipeClient()
                         OpenThreadToken()
                         IsTokenSystem() || IsTokenAdmin()
                           → true → ProcessCommand()
                           → false → RESP_ERROR_ACCESS_DENIED
```

### 8.3 サーバーループとコマンド処理

サーバーループは Overlapped I/O を使用して停止シグナルとの同時待機を実現する:

```
ServerLoop()
  │
  ├── PipeSecurityDescriptor 初期化
  │   失敗 → "IPC disabled" → return (DACL なしでは起動しない)
  │
  └── while (!stopRequested_)
      │
      ├── CreateNamedPipeW(PIPE_NAME, DUPLEX | OVERLAPPED, MESSAGE)
      │   DACL = SYSTEM + Admins only
      │   失敗 → consecutiveFailures++
      │          backoff = min(1000 * 2^(failures-1), 30000) ms
      │          10 回連続失敗で LOG_ERROR
      │
      ├── ConnectNamedPipe(pipeHandle, &overlapped)
      │   ERROR_IO_PENDING → WaitForMultipleObjects:
      │     [0] overlapped.hEvent → クライアント接続
      │     [1] stopEvent_        → サービス停止
      │
      │   WAIT_OBJECT_0 + 1 (stopEvent) → CancelIo → break
      │   WAIT_OBJECT_0 (接続) → HandleClient()
      │
      └── CloseHandle(pipeHandle)
```

#### HandleClient の詳細フロー

```
HandleClient(pipeHandle)
  │
  ├── ReadFile (Overlapped, 5s timeout)
  │   WFMO: overlapped.hEvent + stopEvent_
  │   → IPCMessage { command, dataLength } を読み取り
  │
  ├── AuthorizeClient(pipeHandle, command)
  │   UNAUTHORIZED → SendResponse(RESP_ERROR_ACCESS_DENIED)
  │                  → return
  │
  ├── Data 読み取り (dataLength > 0 の場合)
  │   サイズ検証: >= UNLEAF_MAX_IPC_DATA_SIZE → RESP_ERROR_INVALID_INPUT
  │   ReadFile (Overlapped, 5s timeout)
  │
  ├── ProcessCommand(command, data)
  │   ├── 入力バリデーション (ADD_TARGET, REMOVE_TARGET, SET_INTERVAL)
  │   ├── ハンドラ検索 (handlers_ map)
  │   └── デフォルトハンドラ (GET_STATUS, STOP_SERVICE, GET_LOGS, etc.)
  │
  └── SendResponse(pipe, RESP_SUCCESS, responseData)
      WriteFile(header) → WriteFile(data) → FlushFileBuffers
```

失敗時の復旧: `CreateNamedPipeW` が連続失敗した場合、指数バックオフ (1s → 30s) で再試行する。

### 8.4 メッセージフォーマット

#### リクエスト
```
struct IPCMessage {
    IPCCommand command;    // uint32_t (コマンドID)
    uint32_t dataLength;   // 後続データサイズ
    // [dataLength バイトのデータ]
};
```

#### レスポンス
```
struct IPCResponseMessage {
    IPCResponse response;  // uint32_t (応答コード)
    uint32_t dataLength;   // 後続データサイズ
    // [dataLength バイトのデータ]
};
```

入力バリデーション:
- `CMD_ADD_TARGET` / `CMD_REMOVE_TARGET`: `IsValidProcessName()` による名前検証 (§17.3 参照)
- `CMD_SET_INTERVAL`: 範囲チェック (10 ~ 60,000 ms)
- データサイズ上限: `UNLEAF_MAX_IPC_DATA_SIZE` (65,536 バイト)

### 8.5 文字列変換ユーティリティ (win_string_utils.h)

`ipc_server.cpp` は UTF-8 ↔ ワイド文字列変換にローカル実装ではなく、共通ユーティリティ `win_string_utils.h` を使用する。

```cpp
// src/common/win_string_utils.h
namespace unleaf {
    std::wstring Utf8ToWide(const char* s);   // UTF-8 → std::wstring
    std::string  WideToUtf8(const wchar_t* s); // std::wstring → UTF-8
}
```

| 関数 | 動作 | エラー時 |
|------|------|---------|
| `Utf8ToWide(const char*)` | `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` | `"(conv_error)"` を返す |
| `WideToUtf8(const wchar_t*)` | `WideCharToMultiByte(CP_UTF8, 0)` | `"(conv_error)"` を返す |

`ipc_server.cpp` での使用箇所: `CMD_ADD_TARGET` / `CMD_REMOVE_TARGET` のプロセス名変換、`CMD_GET_STATUS` の JSON 生成における `p.name` のシリアライズ。

---

## 9. 設定管理 (UnLeafConfig)

### 9.1 INI フォーマット

```ini
; UnLeaf Configuration
; Auto-generated - Do not edit while service is running

[Logging]
; Log level: ERROR, ALERT, INFO, DEBUG
LogLevel=INFO
; Log output: 1=enabled, 0=disabled
LogEnabled=1

[Targets]
chrome.exe=1
firefox.exe=1
game.exe=0
```

#### セクション定義

| セクション | キー | 値 | 説明 |
|-----------|------|-----|------|
| `[Logging]` | `LogLevel` | ERROR / ALERT / INFO / DEBUG | ログ出力レベル |
| `[Logging]` | `LogEnabled` | 0 / 1 | ログ出力の有効/無効 |
| `[Targets]` | `<process_name>` | 0 / 1 | ターゲットプロセス (0=無効, 1=有効) |

- BOM 付き UTF-8 に対応 (先頭 3 バイトの自動ストリップ)
- 不明なセクションは警告ログ出力後にスキップ
- 不正なプロセス名 (`IsValidProcessName()` 不合格) はスキップ
- クリティカルプロセス名は `IsCriticalProcess()` でブロック
- ファイルサイズ上限: 1 MB

### 9.2 変更検知

- `FindFirstChangeNotificationW` でベースディレクトリの `FILE_NOTIFY_CHANGE_LAST_WRITE` を監視
- ディレクトリレベルの通知のため、ログファイルの書き込みでも発火する
- **デバウンス**: `CONFIG_DEBOUNCE_MS` (2s) で false positive を抑制
- `HasFileChanged()`: ファイル更新時刻が前回と異なるか確認 (INI ファイル以外の変更を排除)

### 9.3 リロードフロー

```
FindFirstChangeNotification 発火
  │
  ▼
configChangePending_ = true
FindNextChangeNotification() (次回通知を再登録)
  │
  ▼ (2s debounce 経過後)
HandleConfigChange()
  │
  ├── HasFileChanged() → false → return (false positive)
  │
  ├── HasFileChanged() → true
  │     │
  │     ▼
  │   UnLeafConfig::Reload()
  │     │
  │     ▼
  │   ログレベル・有効/無効を反映
  │     │
  │     ▼
  │   RefreshTargetSet() (targetSet_ 更新)
  │     │
  │     ▼
  │   CleanupRemovedTargets() (不要プロセスの追跡解除)
  │     │
  │     ▼
  │   InitialScan() (新ターゲットのスキャン)
```

### 9.4 CleanupRemovedTargets

設定リロード後、ターゲットリストから削除されたプロセスの追跡を解除する。

```
CleanupRemovedTargets()
  │
  ├── targetSet_ のローカルコピーを取得
  │
  ├── CSLockGuard(trackedCs_)
  │   ├── First pass: 有効なルート PID を収集
  │   │   ルートプロセスかつ targetSet_ に存在 → validRootPids
  │   │
  │   └── Second pass: 削除対象を特定
  │       ├── 子プロセス:
  │       │   親が trackedProcesses_ に存在しない AND 自身がターゲット名でない → remove
  │       └── ルートプロセス:
  │           targetSet_ に存在しない → remove
  │
  └── 各 remove 対象: RemoveTrackedProcess(pid)
      → タイマーキャンセル、Wait 解除、メモリ解放
```

**重要**: 子プロセスは親の存在と自身のターゲット名の両方でフィルタされる。例えば chrome.exe をターゲットから外しても、子プロセスの chrome.exe は自身がターゲット名に一致するため残る。

### 9.5 JSON マイグレーション

旧バージョンの `UnLeaf.json` が存在し、`UnLeaf.ini` が存在しない場合、自動的に JSON → INI マイグレーションを実行する。マイグレーション成功後、旧 JSON ファイルは削除される。

---

## 10. RegistryPolicyManager

### 10.1 レジストリパス

UnLeaf は 2 つのレジストリ場所に書き込む:

#### A. EcoQoS 永続除外ポリシー

```
HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerThrottling
  値名: <exe フルパス> (例: C:\Program Files\App\app.exe)
  値型: REG_DWORD
  値:   1
```

#### B. IFEO 優先度設定

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\
  Image File Execution Options\<exe 名>\PerfOptions
  値名: CpuPriorityClass
  値型: REG_DWORD
  値:   3 (HIGH)
```

### 10.2 マニフェスト永続化

レジストリポリシーの適用状態は `UnLeaf_policies.ini` ファイルにマニフェストとして永続化する。

```ini
; UnLeaf Registry Policy Manifest - Auto-managed
; Tracks registry modifications for cleanup on service unregistration
[AppliedPolicies]
chrome.exe=C:\Program Files\Google\Chrome\Application\chrome.exe
```

- **クラッシュ安全性**: マニフェストは **レジストリ書き込みの前** に保存する
  - クラッシュがマニフェスト保存前 → レジストリ未書き込み → リーク無し
  - クラッシュがマニフェスト保存後 → レジストリが不完全でも `RemoveAllPolicies` で冪等クリーンアップ可能

### 10.3 ライフサイクル

| 操作 | メソッド | 呼び出し元 | 説明 |
|------|---------|-----------|------|
| 適用 | `ApplyPolicy()` | EngineCore::ApplyOptimization | プロセス初回検出時 |
| 正常停止クリーンアップ | `CleanupAllPolicies()` | EngineCore::Stop | インメモリ map ベース |
| 登録解除クリーンアップ | `RemoveAllPolicies()` | Manager (サービス登録解除時) | マニフェストベース |
| 初期化 | `Initialize()` | EngineCore::Initialize | マニフェストロード (クラッシュ復旧) |

全操作は **冪等** である。存在しないレジストリキーの削除や空のマニフェストは正常条件として扱う。

---

## 11. ログシステム

### 11.1 ログレベル

| レベル | 値 | ラベル文字 | 用途 |
|--------|-----|-----------|------|
| ERROR | 0 | `E` | 常に出力。致命的エラー |
| ALERT | 1 | `A` | 警告。異常だが回復可能 |
| INFO | 2 | `I` | 通常運用メッセージ (デフォルト) |
| DEBUG | 3 | `D` | 開発詳細。フェーズ遷移、エンフォース結果 |

> **Manager 専用ラベル**: `M` — `LightweightLogger::Manager()` / `LOG_MANAGER` マクロが使用。INFO レベルで出力される。Manager プロセス側の UI 操作ログ (UICallback 経由でログビューにもシアン色で表示) に使用。Service 側では使用しない。

出力判定: `static_cast<uint8_t>(level) <= static_cast<uint8_t>(currentLevel_)`

### 11.2 ローテーション

| 項目 | 値 |
|------|-----|
| ファイル名 | `UnLeaf.log` |
| バックアップ名 | `UnLeaf.log.1` |
| 最大サイズ | 100 KB (`MAX_LOG_SIZE = 102400`) |
| ローテーション方式 | Flush → CloseHandle → `MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)` → 次 write で OPEN_ALWAYS 再オープン |
| ローテーション担当 | **Service プロセスのみ** (`SetRotationEnabled(true)` が必要; Manager はデフォルト `false`) |
| プロセス間排他 | Named Mutex `Global\UnLeafLogRotation` (INFINITE 待機; WAIT_ABANDONED も取得扱い) |
| ファイルオープンモード | `FILE_APPEND_DATA \| FILE_SHARE_READ \| FILE_SHARE_WRITE \| FILE_SHARE_DELETE` |
| FlushFileBuffers | ローテーション直前 (CloseHandle 前) のみ実施 |
| エンコーディング | UTF-8 |

`FILE_SHARE_DELETE` は複数プロセスがファイルを保持した状態で `MoveFileExW` (rename) を成功させるために必須。欠如すると `ERROR_SHARING_VIOLATION` が発生し、以前の実装ではバックアップ未作成のまま `CREATE_ALWAYS` でログが消失していた。

Move 失敗時は `CREATE_ALWAYS` を使用しない。`fileHandle_` を `INVALID_HANDLE_VALUE` のまま残し、次の `WriteMessage()` 呼び出しで `OPEN_ALWAYS` 再オープンすることでデータを保全する。

#### RotationGuard (RAII)

`rotating_` フラグを RAII で管理する内部クラス。コンストラクタで `true` にセット、デストラクタで `false` にリセットするため、いかなる return / 失敗経路でもフラグが確実に戻る。`Log()` 呼び出しはすべて mutex + RotationGuard スコープの外で実行されるため、`rotating_` への依存なしに再入安全性を保証する。

#### Manager のログ収束 (CheckStaleHandle)

Manager プロセスは 100 write ごとに `CheckStaleHandle()` を呼び出す。`GetFileInformationByHandle` で現在のハンドルと `logPath_` のファイル ID (ボリュームシリアル + インデックス) を比較し、不一致 (Service がローテーションして rename した状態) を検知すると `CloseHandle` + `OPEN_ALWAYS` で新しい `UnLeaf.log` に収束する。

### 11.3 動的制御

- **ログレベル変更**: INI の `LogLevel` 値を変更 → 設定リロードで即時反映
- **ログ有効/無効**: INI の `LogEnabled` またはIPCの `CMD_SET_LOG_ENABLED` で切替
- **`enabled_`**: `std::atomic<bool>` で保護。acquire/release セマンティクスで安全にアクセス

### 11.4 出力フォーマット

```
YYYY-MM-DD HH:MM:SS.mmm L メッセージ
```

- タイムスタンプ: `localtime_s` + ミリ秒 (`%Y-%m-%d %H:%M:%S.mmm`)
- ラベル文字 `L`: `E` / `A` / `I` / `D` / `M` の 1 文字
- エンコーディング: UTF-8 (CRLF 改行)

例:
```
2026-02-15 14:30:00.123 I EngineCore started: 3 targets, Event-Driven, SafetyNet=10s
2026-02-15 14:30:01.456 D [TARGET] chrome.exe (PID: 1234) optimized
```

### 11.5 コンソール出力 (SetConsoleOutput)

デバッグビルドでは `Initialize()` 末尾から `SetConsoleOutput(true)` が呼ばれ、`AllocConsole()` でコンソールを確保する。`GetStdHandle` の戻り値を `INVALID_HANDLE_VALUE` で検証し、無効なハンドルは `nullptr` に正規化してから `consoleHandle_` に保存する。

```cpp
// logger.cpp — SetConsoleOutput 実装 (抜粋)
consoleHandle_ = GetStdHandle(STD_OUTPUT_HANDLE);
if (consoleHandle_ == INVALID_HANDLE_VALUE) consoleHandle_ = nullptr;
```

この正規化により、`WriteMessage()` や `Shutdown()` でのヌルチェック (`if (consoleHandle_)`) が正しく動作する。Release ビルドではコンソール確保は行われない。

---

# 第3部: 詳細設計 (Detailed Design)

## 12. EngineCore 詳細設計

### 12.1 Initialize() の各ステップ

```
Initialize(baseDir)
  │
  ├── 1. baseDir_ 設定
  ├── 2. stopEvent_ = CreateEventW(Manual Reset)
  ├── 3. UnLeafConfig::Initialize(baseDir)
  ├── 4. LightweightLogger::Initialize(baseDir)
  ├── 5. ログレベル・有効/無効設定の反映
  ├── 6. RefreshTargetSet() (ターゲット名セット構築)
  ├── 7. timerQueue_ = CreateTimerQueue()
  ├── 8. configChangeHandle_ = FindFirstChangeNotificationW(baseDir, LAST_WRITE)
  ├── 9. safetyNetTimer_ = CreateWaitableTimerW(Auto-Reset)
  ├── 10. enforcementRequestEvent_ = CreateEventW(Auto-Reset)
  ├── 11. hWakeupEvent_ = CreateEventW(Auto-Reset)
  ├── 12. ntdll.dll から NtSetInformationProcess / NtQueryInformationProcess 解決
  ├── 13. RtlGetVersion() で Windows バージョン検出
  ├── 14. RegistryPolicyManager::Initialize(baseDir) (マニフェスト復旧)
  └── 15. ログ出力: "Engine: Initialized (Event-Driven Architecture)"
```

失敗したステップでは `CleanupHandles()` を呼んで確保済みハンドルを解放し、`false` を返す。

### 12.2 Start() の各ステップ

```
Start()
  │
  ├── 1. running_ = true, stopRequested_ = false
  ├── 2. カウンタ初期化 (totalViolations_, 各時刻, startTime_)
  ├── 3. ResetEvent(stopEvent_)
  ├── 4. processMonitor_.Start(processCallback, threadCallback)
  │      成功 → NORMAL モード
  │      失敗 → DEGRADED_ETW モード
  ├── 5. InitialScan() (既存プロセスのスキャン)
  ├── 6. SetWaitableTimer(safetyNetTimer_, 10s periodic)
  └── 7. engineControlThread_ = thread(EngineControlLoop)
```

### 12.3 EngineControlLoop() の詳細

```
EngineControlLoop()
  │
  ├── waitHandles[5] 構築
  │     [1] が INVALID_HANDLE_VALUE の場合、stopEvent_ で代替
  │
  ├── while (!stopRequested_)
  │   │
  │   ├── WaitForMultipleObjects(5, handles, FALSE, INFINITE)
  │   │
  │   ├── WAIT_FAILED → ログ出力 + break
  │   │
  │   ├── switch (waitResult - WAIT_OBJECT_0)
  │   │   ├── WAIT_STOP → break
  │   │   │
  │   │   ├── WAIT_CONFIG_CHANGE
  │   │   │   wakeupConfigChange_++
  │   │   │   configChangeDetected_++
  │   │   │   configChangePending_ = true
  │   │   │   FindNextChangeNotification()
  │   │   │
  │   │   ├── WAIT_SAFETY_NET
  │   │   │   wakeupSafetyNet_++
  │   │   │   HandleSafetyNetCheck()
  │   │   │
  │   │   ├── WAIT_ENFORCEMENT_REQUEST
  │   │   │   wakeupEnforcementRequest_++
  │   │   │   ProcessEnforcementQueue()
  │   │   │
  │   │   └── WAIT_PROCESS_EXIT
  │   │       wakeupProcessExit_++
  │   │       ProcessPendingRemovals()
  │   │
  │   ├── Debounced config reload
  │   │   configChangePending_ && (now - lastConfigCheckTime_ >= 2s)
  │   │   → HandleConfigChange() → configChangePending_ = false
  │   │
  │   └── PerformPeriodicMaintenance(now)
  │       ├── ETW health (30s): restart if unhealthy
  │       ├── Job refresh (5s): RefreshJobObjectPids()
  │       ├── Degraded scan (30s): InitialScanForDegradedMode()
  │       ├── Liveness check (60s): zombie TrackedProcess 検出・除去
  │       └── Stats log (60s): phase breakdown 出力
  │
  └── 最終ドレイン: ProcessPendingRemovals()
```

### 12.4 ApplyOptimization() の詳細

`ApplyOptimization()` は新しいプロセスを最適化し、追跡に追加する中核関数である。ETW イベント (OnProcessStart) または InitialScan から呼ばれる。

```
ApplyOptimization(pid, name, isChild, parentPid)
  │
  ├── Guard checks
  │   ├── IsTracked(pid) → true → return false (二重登録防止)
  │   └── IsCriticalProcess(name) → true → return false (保護プロセス)
  │
  ├── OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION)
  │   = 0x1200 (Chrome サンドボックス互換の最小権限)
  │   失敗 → LOG_DEBUG "[SKIP]..." → return false
  │
  ├── Registry Policy (ルートターゲットのみ: isChild == false)
  │   ├── QueryFullProcessImageNameW → フルパス取得
  │   └── ApplyRegistryPolicy(fullPath, name)
  │       ├── policyAppliedSet_ チェック (冪等)
  │       └── RegistryPolicyManager::ApplyPolicy()
  │           ├── SaveManifest() (クラッシュ安全: レジストリ書き込み前)
  │           ├── DisablePowerThrottling(exePath) → REG_DWORD 1
  │           └── SetIFEOPerfOptions(exeName, 3) → CpuPriorityClass=HIGH
  │
  ├── PulseEnforceV6(hProcess, pid, isIntensive=true)
  │   → 5層防御で即座に EcoQoS 解除
  │
  ├── Job Object (ルートターゲットのみ)
  │   ├── CreateAndAssignJobObject(pid, hProcess)
  │   │   ├── IsProcessInJob → true → "pulse-only mode" (Chrome sandbox)
  │   │   └── CreateJobObjectW → SetInformation(BREAKAWAY_OK) → AssignProcess
  │   └── 子プロセスの場合: 親の Job Object に属しているか確認
  │
  ├── TrackedProcess 構造体の構築
  │   ├── phase = AGGRESSIVE
  │   ├── violationCount = 0
  │   ├── processHandle = ScopedHandle(hProcess)
  │   └── rootTargetPid, inJobObject, jobAssignmentFailed 設定
  │
  ├── プロセス終了監視の登録
  │   ├── OpenProcess(SYNCHRONIZE) → 別ハンドル (SYNCHRONIZE は 0x1200 に含まれない)
  │   ├── WaitCallbackContext{this, pid, tracked} の new 確保
  │   └── RegisterWaitForSingleObject(OnProcessExit, INFINITE, WT_EXECUTEONLYONCE)
  │       失敗 → ハンドル無効化で検知 (Safety Net でカバー)
  │
  ├── trackedProcesses_[pid] = tracked  (CSLockGuard(trackedCs_))
  │   waitContexts_[pid] = context
  │
  └── ScheduleDeferredVerification(pid, step=1)
      → AGGRESSIVE フェーズの遅延検証シーケンス開始
```

**2つのプロセスハンドル**: `processHandle` (0x1200: 制御用) と `waitProcessHandle` (SYNCHRONIZE: 終了検知用) を別々に保持する設計は、Chrome のサンドボックスプロセスが SYNCHRONIZE 権限を許可しない場合にも制御ハンドルを維持するためである。

### 12.5 RemoveTrackedProcess() の詳細

プロセス終了時のクリーンアップは、ロック競合を回避するため「収集→ロック外処理」パターンで実装される。

```
RemoveTrackedProcess(pid)
  │
  │  ──── Phase 1: ロック内でポインタ収集 ────
  │
  ├── CSLockGuard(trackedCs_)
  │   ├── deferredTimer → DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)
  │   │   → deferredCtxToDelete = deferredTimerContext
  │   ├── persistentTimer → DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)
  │   │   → timerCtxToDelete = persistentTimerContext
  │   ├── waitHandleToUnregister = tp->waitHandle
  │   ├── trackedProcesses_.erase(pid)
  │   └── contextToDelete = waitContexts_[pid], erase
  │
  │  ──── Phase 2: ロック外でカーネル操作 ────
  │
  ├── UnregisterWaitEx(waitHandle, INVALID_HANDLE_VALUE) ← blocking
  │
  │  ──── Phase 3: エラー抑制エントリのクリーンアップ ────
  │
  ├── CSLockGuard(trackedCs_)  ← 再取得
  │   └── errorLogSuppression_ から pid のエントリを全削除
  │
  │  ──── Phase 4: メモリ解放 ────
  │
  └── delete contextToDelete, timerCtxToDelete, deferredCtxToDelete
```

`INVALID_HANDLE_VALUE` を `UnregisterWaitEx` と `DeleteTimerQueueTimer` に渡すことで、実行中のコールバックが完了するまで blocking する。これにより use-after-free を防止する。

> **設計制約**: `DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)` は `trackedCs_` を保持したまま呼び出される (§12.5 の収集フロー参照)。コールバック (`DeferredVerifyTimerCallback` / `PersistentEnforceTimerCallback`) がコールバック内で `trackedCs_` を取得しないことが、デッドロック回避の必須前提である。将来これらのコールバックを拡張する場合は、この前提が維持されているかを検証し、デッドロックリスクを評価すること。

### 12.6 HandleSafetyNetCheck() の詳細

Safety Net は **保険チェック** であり、監視メカニズムではない。ETW イベント漏れや OS の EcoQoS 再適用クイークへの最終防御線として機能する。

```
HandleSafetyNetCheck()
  │
  ├── CSLockGuard(trackedCs_)
  │   └── STABLE フェーズのプロセス PID を収集
  │       (AGGRESSIVE は遅延検証中、PERSISTENT はタイマー稼働中なのでスキップ)
  │
  ├── 各 PID について:
  │   └── EnqueueRequest(pid, SAFETY_NET)
  │       → enforcementRequestEvent_ を SetEvent
  │
  └── ProcessEnforcementQueue()  ← 即時処理 (既に制御ループ内)
      │
      └── DispatchEnforcementRequest(SAFETY_NET)
          ├── IsEcoQoSEnabled → OFF → no action
          └── IsEcoQoSEnabled → ON (STABLE のみ)
              ├── PulseEnforceV6
              ├── violationCount++
              ├── violationCount < 3 → AGGRESSIVE
              └── violationCount >= 3 → PERSISTENT
```

**重要**: Safety Net は `HandleSafetyNetCheck()` から直接 `ProcessEnforcementQueue()` を呼ぶため、キューを経由した後すぐに処理される。WFMO の次回 wakeup を待つ必要がない。

### 12.7 OnProcessStart() / OnThreadStart() の詳細

#### OnProcessStart (ETW Process Start コールバック)

```
OnProcessStart(pid, parentPid, imageName)
  │
  ├── stopRequested_ → return
  ├── IsCriticalProcess(imageName) → return
  │
  ├── Case 1: IsTrackedParent(parentPid) → true
  │   └── ApplyOptimization(pid, imageName, isChild=true, parentPid)
  │       (親プロセスの子として追跡)
  │
  └── Case 2: IsTargetName(imageName) → true
      └── ApplyOptimization(pid, imageName, isChild=false, parentPid=0)
          (ルートターゲットとして追跡)
```

子プロセス検出は親 PID ベースで行う。これにより、ターゲットリストに無いプロセス名であっても、ターゲットプロセスの子であれば自動的に追跡される。

#### OnThreadStart (ETW Thread Start コールバック)

```
OnThreadStart(threadId, ownerPid)
  │
  ├── stopRequested_ → return
  │
  ├── CSLockGuard(trackedCs_)
  │   └── trackedProcesses_.find(ownerPid)
  │       未追跡 → return (O(1) フィルタ)
  │       追跡中 → currentPhase を取得
  │
  ├── AGGRESSIVE → return (遅延検証がカバー)
  │
  └── STABLE / PERSISTENT
      └── EnqueueRequest(ownerPid, ETW_THREAD_START)
```

Thread Start イベントは非常に頻繁に発火するため、`trackedProcesses_.find()` による O(1) フィルタが重要。非追跡プロセスのスレッド生成を即座にスキップする。

### 12.8 Stop() 9ステップ シャットダウンシーケンス

```
図5: Stop() 9ステップ シャットダウンシーケンス

  Stop()
  │
  ├── Step 0: compare_exchange_strong(running_, true→false)
  │           (二重停止防止: 既に false なら即 return)
  │
  ├── Step 1: stopRequested_ = true
  │           SetEvent(stopEvent_)
  │           → WFMO の WAIT_STOP をトリガー
  │
  ├── Step 2: processMonitor_.Stop()
  │           → CloseTrace (ProcessTrace ブロック解除)
  │           → ControlTrace(STOP)
  │           → consumerThread_.join()
  │
  ├── Step 3: engineControlThread_.join()
  │           → EngineControlLoop が WAIT_STOP で break 済み
  │
  ├── Step 4: Timer contexts 収集
  │           CSLockGuard(trackedCs_)
  │           全 TrackedProcess の persistentTimerContext / deferredTimerContext を
  │           vector に移動 (ポインタを nullptr に設定)
  │
  ├── Step 5: DeleteTimerQueueEx(timerQueue_, INVALID_HANDLE_VALUE)
  │           INVALID_HANDLE_VALUE = 全コールバック完了まで blocking
  │           → timer contexts を delete
  │
  ├── Step 6: Wait handles unregister
  │           CSLockGuard(trackedCs_)
  │           全 TrackedProcess の waitHandle を収集
  │           waitContexts_ クリア
  │           trackedProcesses_ クリア
  │           → UnregisterWaitEx(h, INVALID_HANDLE_VALUE) (各ハンドル)
  │           → WaitCallbackContext* を delete
  │
  ├── Step 7: CleanupJobObjects()
  │           → jobObjects_.clear() (デストラクタで CloseHandle)
  │
  ├── Step 8: RegistryPolicyManager::CleanupAllPolicies()
  │           → 全レジストリエントリ削除
  │           → マニフェストファイル削除
  │           → policyAppliedSet_ クリア
  │
  └── Step 9: CleanupHandles()
              → stopEvent_, timerQueue_, configChangeHandle_,
                safetyNetTimer_, enforcementRequestEvent_,
                hWakeupEvent_ を順に CloseHandle/解放
```

**重要な設計ポイント**:

- `INVALID_HANDLE_VALUE` を `DeleteTimerQueueEx` と `UnregisterWaitEx` に渡すことで、実行中のコールバックが完了するまで blocking する。これによりコールバック中のメモリアクセス違反を防止する。
- Step 4 で Timer contexts を収集し Step 5 で Timer Queue 破棄後に delete する。逆順だと delete 済みメモリへのアクセスが発生する。
- `compare_exchange_strong` によるアトミックな停止権取得で、並行 `Stop()` 呼び出しを安全に処理する。

---

### 12.9 PerformPeriodicMaintenance() の詳細

定期保守タスクは専用のタイマーを持たず、他の WFMO wakeup に「便乗」して実行される。各タスクは独自のインターバルで最終実行時刻を管理する。

```
PerformPeriodicMaintenance(now)
  │
  ├── ETW ヘルスチェック (30s ごと)
  │   ├── operationMode_ == NORMAL && !processMonitor_.IsHealthy()
  │   │   ├── processMonitor_.Stop()
  │   │   ├── processMonitor_.Start(callbacks)
  │   │   │   成功 → LOG "Session restarted successfully"
  │   │   │   失敗 → operationMode_ = DEGRADED_ETW
  │   │   │          LOG "Restart failed - switching to DEGRADED mode"
  │   │   └── 成功しても失敗しても lastEtwHealthCheck_ = now
  │   └── NORMAL && healthy → skip
  │
  ├── Job Object PID リフレッシュ (5s ごと)
  │   ├── jobObjects_.empty() → skip (ロック取得のみで判定)
  │   └── RefreshJobObjectPids() → 新しい子プロセスを検出
  │
  ├── DEGRADED_ETW フォールバックスキャン (30s ごと)
  │   └── InitialScanForDegradedMode()
  │       Toolhelp32 で全プロセスをスキャン
  │       ターゲット名 or 追跡中の親 PID にマッチ → ApplyOptimization
  │
  ├── プロセス生存チェック (60s ごと)
  │   └── waitHandle == nullptr のエントリのみ対象
  │       OpenProcess + GetExitCodeProcess で確認
  │       終了済み → pendingRemovalPids_ に push → hWakeupEvent_
  │
  ├── errorLogSuppression_ TTL クリーンアップ (60s ごと, `SUPPRESSION_CLEANUP_INTERVAL`)
  │   └── trackedCs_ ロック内で全エントリを走査
  │       now - entry.second >= SUPPRESSION_TTL (300s) → erase
  │       size() > SUPPRESSION_MAX_SIZE (2000) → oldest-first eviction
  │       削除あり → LOG_DEBUG "[MAINT] errSup: N -> M"
  │
  ├── [MEM] メモリ診断ログ (起動後 30 分: 10s, 以降: 60s)
  │   └── 条件: LOG_DEBUG レベル時のみ出力
  │       GetProcessMemoryInfo() → pmcOk (bool)
  │       GetProcessHandleCount() → hcOk (bool)
  │       出力: [MEM] pid=N priv=N rss=N commit=N handles=N policy=N errSup=N
  │
  └── 統計ログ出力 (60s ごと)
      └── 条件: count > 0 && (aggressiveCount > 0 || persistentCount > 0)
          (全プロセスが STABLE の場合はログを出力しない)
          Stats: N tracked (A:x S:y P:z), M jobs, viol=V, wakeup(...)
```

### 12.10 InitialScan() の詳細

`InitialScan()` は Start() 時および設定リロード後に呼ばれ、既に起動しているターゲットプロセスを検出する。

```
InitialScan()
  │
  ├── CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)
  ├── ScopedSnapshot で RAII 管理
  │
  ├── Phase 1: 全プロセスマップ構築
  │   ProcessMap: PID → { name, parentPid }
  │
  ├── Phase 2: ターゲット検出 + 子孫プロセスの再帰収集
  │   │
  │   ├── collectDescendants(parentPid, out):
  │   │   processMap 内で parentPid を親に持つプロセスを再帰的に収集
  │   │   クリティカルプロセスはスキップ
  │   │
  │   └── 各ターゲットプロセス:
  │       ├── ApplyOptimization(pid, name, isChild=false)
  │       └── 子孫プロセスを収集:
  │           └── ApplyOptimization(childPid, childName, isChild=true, rootPid)
```

**InitialScanForDegradedMode との違い**:
- `InitialScan`: 子孫プロセスの再帰的な収集を行う (ツリー構造を認識)
- `InitialScanForDegradedMode`: 直接の親子関係のみチェック (フラットスキャン)

---

## 13. メモリ管理・ライフタイム管理

### 13.1 shared_ptr によるプロセス管理

`TrackedProcess` は `std::shared_ptr<TrackedProcess>` で管理される:

```cpp
std::map<DWORD, std::shared_ptr<TrackedProcess>> trackedProcesses_;
```

共有所有権が必要な理由:

| 所有者 | 用途 |
|--------|------|
| `trackedProcesses_` map | プライマリ所有権 |
| `WaitCallbackContext` | `OnProcessExit` コールバック中のアクセス保証 |
| `DeferredVerifyContext` | Timer コールバック中のアクセス保証 |

`shared_ptr` により、Timer コールバック実行中にプロセスが `trackedProcesses_` から削除されても、`TrackedProcess` インスタンスが premature destruction されない。

### 13.2 Context ライフサイクル

#### WaitCallbackContext

```
生成: ApplyOptimization() → new WaitCallbackContext{this, pid, tracked}
保存: waitContexts_[pid] = context
破棄:
  - RemoveTrackedProcess() → waitContexts_.erase() → delete
  - Stop() Step 6 → 全 context を delete
```

#### DeferredVerifyContext

```
生成: ScheduleDeferredVerification() → new DeferredVerifyContext{engine, pid, step, process}
保存: tp->deferredTimerContext = context  (one-shot)
      tp->persistentTimerContext = context (recurring)
破棄:
  - DispatchEnforcementRequest(DEFERRED_VERIFICATION) → delete tp->deferredTimerContext
  - CancelProcessTimers() → DeleteTimerQueueTimer(INVALID_HANDLE_VALUE) → delete context
  - Stop() Step 4-5 → 収集 → Timer Queue 破棄 → delete
```

**one-shot タイマー**: コールバック発火後、DispatchEnforcementRequest 内で context を delete する。
**recurring タイマー**: context は再利用されるため、コールバック内では delete しない。

### 13.3 ScopedHandle 一覧

| 型名 | 対象 | Deleter |
|------|------|---------|
| `ScopedHandle` | 一般的な `HANDLE` | `CloseHandle` |
| `ScopedSnapshot` | Toolhelp32 スナップショット | `CloseHandle` |
| `ScopedSCMHandle` | Service Control Manager | `CloseServiceHandle` |
| `WaitHandle` | `RegisterWaitForSingleObject` | `UnregisterWaitEx(h, INVALID_HANDLE_VALUE)` |
| `CriticalSection` | `CRITICAL_SECTION` | `DeleteCriticalSection` |
| `CSLockGuard` | `CriticalSection` の RAII ロック | `LeaveCriticalSection` |

`MakeScopedHandle(h)`: `nullptr` と `INVALID_HANDLE_VALUE` を `nullptr` に正規化して `ScopedHandle` を構築する。

### 13.4 コンテナ上限管理 (§8.22)

長時間稼働でコンテナが肥大化しないよう、2 つのコンテナに上限機構を実装している。

| コンテナ | 型 | 上限機構 | 緊急キャップ |
|---------|-----|---------|-------------|
| `errorLogSuppression_` | `map<pair<DWORD,DWORD>, ULONGLONG>` | 60s ごと TTL クリーンアップ (TTL=300s) | 2,000 エントリ超過で先頭 (oldest) から削除 |
| `policyAppliedSet_` | `set<std::wstring>` | config reload 時に swap クリア | 1,000 エントリ超過で swap クリア |

`errorLogSuppression_` は `trackedCs_` で保護される。`PerformPeriodicMaintenance()` が `SUPPRESSION_CLEANUP_INTERVAL` (60s) ごとに TTL 切れエントリを除去し、緊急キャップを `SUPPRESSION_MAX_SIZE` (2,000) で適用する。

### 13.5 engine_logic 純粋ロジックモジュール (§8.40〜§8.41)

§8.40 で `src/engine/` ディレクトリを新設し、Win32 依存のない純粋 C++ 決定ロジックを `EngineCore` から分離した。

| 関数 | シグネチャ概要 | 用途 |
|------|--------------|------|
| `IsTargetProcess` | `bool(const wstring&, const set<wstring>&)` | targetSet_ 検索 |
| `IsCacheValid` | `bool(bool, uint64_t, uint64_t, uint64_t)` | EcoQoS マイクロキャッシュ有効性判定 |
| `ShouldExitPersistent` | `bool(uint64_t, uint64_t)` | PERSISTENT → STABLE 遷移判定 (60s clean) |
| `NextPhaseOnViolation` | `ProcessPhase(uint32_t, const EnginePolicy&) noexcept` | 違反後フェーズ決定 |
| `DeferredVerifyDelayMs` | `uint32_t(uint8_t, const EnginePolicy&) noexcept` | 遅延検証タイマー遅延計算 |

全関数は **Win32 API ゼロ** の純粋 C++ 実装であり、`tests/test_engine_logic.cpp` (32 テストケース) でカバーされている。`EnginePolicy` 構造体のテストは `tests/test_engine_policy.cpp` (2 テストケース) に分離されている。

---

## 14. 同期・排他制御

### 14.1 CriticalSection 一覧

| 変数名 | 所属クラス | 保護対象 |
|--------|-----------|---------|
| `trackedCs_` | EngineCore | `trackedProcesses_`, `waitContexts_`, `errorLogSuppression_` |
| `queueCs_` | EngineCore | `enforcementQueue_` |
| `pendingRemovalCs_` | EngineCore | `pendingRemovalPids_` |
| `targetCs_` | EngineCore | `targetSet_` |
| `jobCs_` | EngineCore | `jobObjects_` |
| `policySetCs_` | EngineCore | `policyAppliedSet_` |
| `handlerCs_` | IPCServer | `handlers_` |
| `cs_` | UnLeafConfig | `targets_`, `configPath_`, `logLevel_` 等 |
| `cs_` | LightweightLogger | `fileHandle_`, `initialized_`, `rotationEnabled_`, `rotating_` 等 |
| `policyCs_` | RegistryPolicyManager | `appliedPolicies_`, `manifestPath_` |

### 14.2 プロセス間 Mutex

| 名前 | 用途 | 取得箇所 |
|------|------|---------|
| `Global\UnLeafLogRotation` | ログローテーション排他 | `LightweightLogger::CheckRotation()` (Service のみ) |

INFINITE 待機。`WAIT_ABANDONED` (前回保持プロセスが異常終了) の場合も取得とみなしてローテーションを継続する。Mutex ハンドルは `Initialize()` で `CreateMutexW` により確保し、`Shutdown()` で `CloseHandle`。

### 14.3 ロック順序 (プロセス内)

デッドロックを回避するため、複数のロックを取得する場合は以下の順序を守る:

```
jobCs_ → trackedCs_ (RefreshJobObjectPids で使用)
```

その他のロックは同時取得されないか、単独で使用される。

### 14.4 atomic パターン

| 変数 | 型 | 用途 |
|------|-----|------|
| `running_` | `atomic<bool>` | エンジン実行状態 |
| `stopRequested_` | `atomic<bool>` | 停止要求フラグ |
| `totalViolations_` | `atomic<uint32_t>` | 累計 violation 数 |
| `totalRetries_` | `atomic<uint32_t>` | エラーリトライ累計 |
| `totalHandleReopen_` | `atomic<uint32_t>` | ハンドル再オープン累計 |
| `wakeupConfigChange_` | `atomic<uint32_t>` | Config Change wakeup 回数 |
| `wakeupSafetyNet_` | `atomic<uint32_t>` | Safety Net wakeup 回数 |
| `wakeupEnforcementRequest_` | `atomic<uint32_t>` | Enforcement wakeup 回数 |
| `wakeupProcessExit_` | `atomic<uint32_t>` | Process Exit wakeup 回数 |
| `persistentEnforceApplied_` | `atomic<uint32_t>` | PERSISTENT enforce 適用数 |
| `persistentEnforceSkipped_` | `atomic<uint32_t>` | PERSISTENT enforce スキップ数 |
| `error5Count_` | `atomic<uint32_t>` | ERROR_ACCESS_DENIED 累計 |
| `error87Count_` | `atomic<uint32_t>` | ERROR_INVALID_PARAMETER 累計 |
| `shutdownWarnings_` | `atomic<uint32_t>` | シャットダウン時の警告数 |
| `ntApiSuccessCount_` | `atomic<uint32_t>` | NT API 成功数 |
| `ntApiFailCount_` | `atomic<uint32_t>` | NT API 失敗数 |
| `policyApplyCount_` | `atomic<uint32_t>` | ポリシー適用数 |
| `etwThreadDeduped_` | `atomic<uint32_t>` | ETW Thread 重複排除数 |
| `enforceCount_` | `atomic<uint32_t>` | PulseEnforceV6 総呼び出し数 |
| `enforceSuccessCount_` | `atomic<uint32_t>` | 成功数 |
| `enforceFailCount_` | `atomic<uint32_t>` | 失敗数 |
| `enforceLatencySumUs_` | `atomic<uint64_t>` | レイテンシ合計 (μs) |
| `enforceLatencyMaxUs_` | `atomic<uint32_t>` | 最大レイテンシ (μs) |
| `lastEnforceTimeMs_` | `atomic<uint64_t>` | 最終エンフォース時刻 (Unix Epoch ms) |
| `configChangeDetected_` | `atomic<uint32_t>` | 設定変更検知数 |
| `configReloadCount_` | `atomic<uint32_t>` | 設定リロード数 |
| `enabled_` (Logger) | `atomic<bool>` | ログ出力有効/無効 (acquire/release) |

統計カウンタは `memory_order_relaxed` で十分 (厳密な順序は不要)。
`stopRequested_` は `memory_order_seq_cst` (デフォルト) で、停止シグナルの可視性を保証する。

---

## 15. エラー処理・自己修復

### 15.1 HandleEnforceError

HandleEnforceError は PulseEnforceV6 が失敗した際に呼ばれ、エラーの種類に応じた自己修復を行う。

```
HandleEnforceError(hProcess, pid, error)
  │
  ├── totalRetries_.fetch_add(1, relaxed)
  ├── エラーコード別カウンタ更新
  │   error == 5  → error5Count_++
  │   error == 87 → error87Count_++
  │
  ├── プロセス生存確認
  │   GetExitCodeProcess(hProcess, &exitCode)
  │   exitCode != STILL_ACTIVE → processHandle.reset() → return
  │   (Safety Net がプロセス再検出を試みる)
  │
  ├── CSLockGuard(trackedCs_) ← TrackedProcess の更新はロック内
  │   ├── tp->consecutiveFailures++
  │   └── tp->lastErrorCode = error
  │
  ├── エラーログ抑制チェック
  │   キー: (pid, error)
  │   errorLogSuppression_ map で 60s 以内の重複ログを抑制
  │   → shouldLog = true/false
  │
  └── switch (error)
      │
      ├── ERROR_ACCESS_DENIED (5)
      │   consecutiveFailures <= 2
      │     → nextRetryTime = now + 50ms (RETRY_BACKOFF_BASE_MS)
      │     → 次回の enforcement で再試行
      │   consecutiveFailures > 2
      │     → give up (shouldLog なら "[GIVE_UP]" ログ)
      │     → プロセスは trackedProcesses_ に残り続ける
      │       (Priority は Set 済みで部分的な保護は維持)
      │
      ├── ERROR_INVALID_HANDLE (6)
      │   → processHandle.reset()
      │   → ハンドルの即時破棄
      │   → ReopenProcessHandle() による自動回復は別パスで行う
      │
      ├── ERROR_INVALID_PARAMETER (87)
      │   → processHandle.reset()
      │   → プロセスは既に終了している可能性が高い
      │   → Wait コールバック経由で RemoveTrackedProcess が呼ばれる
      │
      └── その他のエラー
          consecutiveFailures <= MAX_RETRY_COUNT (5)
            → backoff = 50ms × 2^(failures-1)
            → nextRetryTime = now + backoff
            → 50ms → 100ms → 200ms → 400ms → 800ms
          consecutiveFailures > 5
            → give up (shouldLog なら "[GIVE_UP]" ログ)
```

### 15.1.1 ReopenProcessHandle

ハンドルが無効化された場合の自動回復メカニズム:

```
ReopenProcessHandle(pid)
  │
  ├── totalHandleReopen_++
  ├── OpenProcess(0x1200, pid) → 新しい制御ハンドル
  │   失敗 → return false
  │
  ├── CSLockGuard(trackedCs_)
  │   ├── 古い waitHandle を収集
  │   ├── waitProcessHandle.reset()
  │   ├── processHandle = MakeScopedHandle(新ハンドル)
  │   ├── consecutiveFailures = 0, nextRetryTime = 0 (カウンタリセット)
  │   ├── OpenProcess(SYNCHRONIZE) → 新しい待機ハンドル
  │   └── RegisterWaitForSingleObject(OnProcessExit) → 新 WaitCallbackContext
  │
  ├── (ロック外) UnregisterWaitEx(旧waitHandle, INVALID_HANDLE_VALUE)
  └── delete oldContext
```

### 15.2 リトライ戦略

| エラー | リトライ回数 | バックオフ | 最終処理 |
|--------|-------------|-----------|---------|
| ACCESS_DENIED (5) | 2 回 | 50ms 固定 | give up (ログ) |
| INVALID_HANDLE | 0 回 | - | ハンドル reset |
| INVALID_PARAMETER (87) | 0 回 | - | ハンドル reset |
| その他 | 5 回 | 50ms × 2^n | give up (ログ) |

### 15.3 ETW 復旧

```
PerformPeriodicMaintenance() (30s ごと)
  │
  ├── operationMode_ == NORMAL && !processMonitor_.IsHealthy()
  │   │
  │   ├── processMonitor_.Stop()
  │   ├── processMonitor_.Start(callbacks)
  │   │   成功 → LOG "Session restarted successfully"
  │   │   失敗 → operationMode_ = DEGRADED_ETW
  │   │           LOG "Restart failed - switching to DEGRADED mode"
  │
  └── operationMode_ == DEGRADED_ETW
      │
      └── 30s ごとに InitialScanForDegradedMode()
          (Toolhelp32 でプロセス一覧スキャン)
```

### 15.4 縮退運転 (OperationMode)

| モード | 条件 | 動作 |
|--------|------|------|
| `NORMAL` | ETW 正常 | ETW イベント駆動 + Safety Net |
| `DEGRADED_ETW` | ETW 障害 | 30s ごとの Toolhelp32 フォールバックスキャン |
| `DEGRADED_CONFIG` | 設定読み込み失敗 | 限定機能 (未使用) |

### 15.5 エラーログ抑制

同一 PID × 同一エラーコードのログ出力は 60 秒間に 1 回のみ:

```
errorLogSuppression_: map<pair<DWORD, DWORD>, ULONGLONG>
  キー: (pid, errorCode)
  値:   最終ログ出力時刻

shouldLog = (now - lastLogTime >= ERROR_LOG_SUPPRESS_MS)
```

プロセスが `RemoveTrackedProcess()` で削除される際、対応する抑制エントリもクリーンアップされる。

---

## 16. Job Object 管理

### 16.1 作成条件

`CreateAndAssignJobObject()` は以下の条件を **すべて** 満たす場合にのみ Job Object を作成する:

1. ルートターゲットプロセス (`isChild == false`)
2. プロセスが既に Job Object に属していない (`IsProcessInJob()` → `FALSE`)

**既に Job に属している場合** (Chrome サンドボックスなど):

- Job Object の作成はスキップされる
- `TrackedProcess` は作成され、`jobAssignmentFailed = true` がセットされる
- PulseEnforceV6 による EcoQoS 解除は継続する ("pulse-only mode")

Job Object 設定:
```
CreateJobObjectW(nullptr, nullptr)  // 無名 Job Object

JOBOBJECT_EXTENDED_LIMIT_INFORMATION:
  LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK
             | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK
```

| フラグ | 意味 |
|--------|------|
| `BREAKAWAY_OK` | `CREATE_BREAKAWAY_FROM_JOB` 指定の子プロセスが Job から離脱可能 |
| `SILENT_BREAKAWAY_OK` | 子プロセスがフラグ指定なしでも暗黙的に Job から離脱可能 |

これらのフラグにより、アプリケーション自身の子プロセス管理 (GPU プロセス、レンダラー等) を阻害しない。

### 16.1.1 JobObjectInfo 構造体

```cpp
struct JobObjectInfo {
    HANDLE jobHandle;     // カーネル Job Object ハンドル
    DWORD  rootPid;       // この Job を作成したルートプロセスの PID
    bool   isOwnJob;      // true = UnLeaf が作成した Job
};
```

- Non-copyable, movable
- デストラクタで `CloseHandle(jobHandle)` を保証
- `jobObjects_`: `map<DWORD, unique_ptr<JobObjectInfo>>` で管理

### 16.2 RefreshJobObjectPids 2-pass アルゴリズム

5 秒ごとに `PerformPeriodicMaintenance` から呼ばれ、Job Object 内の新しい子プロセスを検出する。

```
RefreshJobObjectPids()
  │
  │  ════════════════════════════════════════
  │  Pass 1: ロック保持 (データ収集のみ)
  │  ════════════════════════════════════════
  │
  ├── CSLockGuard(jobCs_)         ← 順序 1
  ├── CSLockGuard(trackedCs_)     ← 順序 2 (ロック順序は jobCs_ → trackedCs_)
  │
  ├── 各 JobObjectInfo (isOwnJob == true のみ):
  │   │
  │   ├── QueryInformationJobObject(JobObjectBasicProcessIdList)
  │   │   ├── スタック上の固定長バッファ:
  │   │   │   BYTE buffer[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)
  │   │   │              + (MAX_JOB_PIDS - 1) * sizeof(ULONG_PTR)]
  │   │   │   (MAX_JOB_PIDS = 1024: 最大 1024 プロセス分)
  │   │   │   動的アロケーションなし (パフォーマンス + 例外安全)
  │   │   │
  │   │   └── 失敗 → この Job をスキップ (ハンドル無効化等)
  │   │
  │   ├── PID リストを走査:
  │   │   trackedProcesses_.find(pid) == end → newPids に追加
  │   │
  │   └── JobQueryResult { rootPid, newPids[] }
  │       newPids が空なら jobResults に追加しない
  │
  │  ════════════════════════════════════════
  │  Pass 2: ロック解放 (カーネル操作 + 最適化)
  │  ════════════════════════════════════════
  │
  └── 各 newPid:
      ├── IsTracked(pid) → true → skip
      │   (Pass 1-2 間に別スレッドで追加された可能性がある: 二重チェック)
      │
      ├── OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
      │   失敗 → skip (プロセス終了済み)
      │
      ├── QueryFullProcessImageNameW → フルパスからファイル名抽出
      │   pos = fullPath.find_last_of("\\/")
      │   name = fullPath.substr(pos + 1)
      │
      ├── IsCriticalProcess(name) → skip
      │
      └── ApplyOptimization(pid, name, isChild=true, rootPid=result.rootPid)
          → PulseEnforceV6 + TrackedProcess 登録 + 遅延検証開始
```

**2-pass の設計意図**:
- Pass 1 ではカーネル呼び出し (`QueryInformationJobObject`) とメモリ操作のみで、`OpenProcess` や `ApplyOptimization` のような重い処理を避ける
- Pass 2 でロックを解放してから `OpenProcess` / `ApplyOptimization` を実行することで、他のスレッド (ETW コールバック等) のロック待機時間を最小化する
- ロック順序 (`jobCs_` → `trackedCs_`) をプロジェクト全体で統一し、デッドロックを防止する

### 16.3 クリーンアップ

```
CleanupJobObjects()
  │
  ├── CSLockGuard(jobCs_)
  └── jobObjects_.clear()
      → JobObjectInfo デストラクタ: CloseHandle(jobHandle)
```

`JobObjectInfo` は non-copyable, movable。デストラクタで `CloseHandle` を保証する。

---

## 17. セキュリティ

### 17.1 DACL 構成 (PipeSecurityDescriptor)

Named Pipe に適用される DACL:

| ACE | SID | 権限 |
|-----|-----|------|
| #0 | SYSTEM (S-1-5-18) | GENERIC_READ \| GENERIC_WRITE |
| #1 | Administrators (S-1-5-32-544) | GENERIC_READ \| GENERIC_WRITE |

- `SECURITY_ATTRIBUTES` を `CreateNamedPipeW` に渡す
- DACL 初期化に失敗した場合、IPC サーバーは **起動しない** (セキュリティリスク回避)

### 17.2 認可フロー

```
AuthorizeClient(pipeHandle, cmd)
  │
  ├── GetCommandPermission(cmd)
  │   ├── PUBLIC  → return AUTHORIZED (即許可)
  │   ├── ADMIN   → トークン検証必要
  │   └── SYSTEM_ONLY → トークン検証必要
  │
  ├── ImpersonateNamedPipeClient(pipeHandle)
  │   失敗 → return ERROR_IMPERSONATION
  │
  ├── OpenThreadToken(GetCurrentThread(), TOKEN_QUERY)
  │   失敗 → RevertToSelf() → return ERROR_TOKEN
  │
  ├── ADMIN:
  │   IsTokenAdmin(hToken) → CheckTokenMembership(adminSid)
  │
  ├── SYSTEM_ONLY:
  │   IsTokenSystem(hToken) || IsTokenAdmin(hToken)
  │
  ├── CloseHandle(hToken)
  ├── RevertToSelf()
  └── return AUTHORIZED / UNAUTHORIZED
```

### 17.3 入力バリデーション

#### プロセス名検証 (IsValidProcessName)

```
IsValidProcessName(name)
  ├── 空文字列チェック → false
  ├── 長さ上限チェック (MAX_PROCESS_NAME_LEN = 260) → false
  ├── パストラバーサル ("..") → false
  ├── 絶対パス検出 ("C:\" 等) → false
  ├── ディレクトリセパレータ検出 ("\", "/") → false
  ├── 文字種チェック: 英数字, '_', '.', '-' のみ → false
  └── ".exe" 末尾チェック → false (不一致時)
```

#### IPC データサイズ検証

- `dataLength >= UNLEAF_MAX_IPC_DATA_SIZE (65536)` → `RESP_ERROR_INVALID_INPUT`
- `CMD_SET_INTERVAL`: 4 バイト固定長、値範囲 10 ~ 60,000 ms

#### クリティカルプロセス保護

`IsCriticalProcess()` により、システム重要プロセスのターゲット登録をブロックする (§D 参照)。

---

# 付録

## A. 定数一覧

### A.1 バージョン・識別子 (types.h)

| 定数名 | 型 | 値 |
|--------|-----|-----|
| `VERSION` | `const wchar_t*` | `L"1.00"` |
| `SERVICE_NAME` | `const wchar_t*` | `L"UnLeafService"` |
| `SERVICE_DISPLAY_NAME` | `const wchar_t*` | `L"UnLeaf Service"` |
| `SERVICE_DESCRIPTION` | `const wchar_t*` | `L"Optimization Engine (Native C++ Edition)"` |
| `PIPE_NAME` | `const wchar_t*` | `L"\\\\.\\pipe\\UnLeafServicePipe"` |

### A.2 ファイル・バッファ制限 (types.h)

| 定数名 | 値 | 説明 |
|--------|-----|------|
| `CONFIG_FILENAME` | `L"UnLeaf.ini"` | 設定ファイル名 |
| `CONFIG_FILENAME_OLD` | `L"UnLeaf.json"` | 旧形式 (マイグレーション用) |
| `LOG_FILENAME` | `L"UnLeaf.log"` | ログファイル名 |
| `LOG_BACKUP_FILENAME` | `L"UnLeaf.log.1"` | ログバックアップ名 |
| `MAX_LOG_SIZE` | 102,400 | 最大ログサイズ (100KB) |
| `UNLEAF_MAX_IPC_DATA_SIZE` | 65,536 | IPC 最大データサイズ |
| `UNLEAF_MAX_LOG_READ_SIZE` | 8,192 | ログ読み取り最大サイズ/回 |
| `UNLEAF_MAX_PROCESS_NAME_LEN` | 260 | プロセス名最大長 |

### A.3 EcoQoS 制御定数 (types.h)

| 定数名 | 値 | 説明 |
|--------|-----|------|
| `UNLEAF_THROTTLE_VERSION` | 1 | PROCESS_POWER_THROTTLING_STATE Version |
| `UNLEAF_THROTTLE_EXECUTION_SPEED` | 0x1 | 実行速度制御フラグ |
| `UNLEAF_THROTTLE_IGNORE_TIMER` | 0x4 | タイマー無視フラグ (Win11) |
| `UNLEAF_PROCESS_POWER_THROTTLING` | 4 | ProcessInformationClass |
| `NT_PROCESS_POWER_THROTTLING_STATE` | 77 | NtApi ProcessInformationClass |
| `UNLEAF_TARGET_PRIORITY` | HIGH_PRIORITY_CLASS | ターゲット優先度 |
| `UNLEAF_MIN_PRIORITY` | NORMAL_PRIORITY_CLASS | 最低許容優先度 |

### A.4 スレッド制御定数 (types.h)

| 定数名 | 値 | 説明 |
|--------|-----|------|
| `UNLEAF_THREAD_THROTTLE_VERSION` | 1 | THREAD_POWER_THROTTLING_STATE Version |
| `UNLEAF_THREAD_THROTTLE_EXECUTION_SPEED` | 0x1 | 実行速度制御フラグ |
| `UNLEAF_THREAD_POWER_THROTTLING` | 4 | ThreadInformationClass |

### A.5 リトライ・制限定数 (types.h)

| 定数名 | 値 | 説明 |
|--------|-----|------|
| `MAX_TRACKED_PROCESSES` | 256 | 最大追跡プロセス数 |
| `MAX_JOB_PIDS` | 1,024 | Job Object 最大 PID 数 |
| `MAX_RETRY_COUNT` | 5 | 最大リトライ回数 |
| `RETRY_BACKOFF_BASE_MS` | 50 | リトライバックオフ基準 (ms) |
| `UNLEAF_MIN_INTERVAL_MS` | 10 | 最小インターバル (IPC) |
| `UNLEAF_MAX_INTERVAL_MS` | 60,000 | 最大インターバル (IPC) |
| `WINDOWS_11_BUILD_THRESHOLD` | 22,000 | Windows 11 最小ビルド番号 |

### A.6 NT ステータスコード (types.h)

| 定数名 | 値 | 説明 |
|--------|-----|------|
| `STATUS_SUCCESS` | 0x00000000 | 成功 |
| `STATUS_INFO_LENGTH_MISMATCH` | 0xC0000004 | バッファサイズ不足 |
| `STATUS_ACCESS_DENIED` | 0xC0000022 | アクセス拒否 |

### A.7 タイミング定数 (engine_core.h)

| 定数名 | 値 (ms) | 説明 |
|--------|---------|------|
| `AGGRESSIVE_DURATION` | 3,000 | AGGRESSIVE 持続時間 |
| `DEFERRED_VERIFY_1` | 200 | 遅延検証 Step 1 |
| `DEFERRED_VERIFY_2` | 1,000 | 遅延検証 Step 2 |
| `DEFERRED_VERIFY_FINAL` | 3,000 | 遅延検証 Step 3 (最終) |
| `PERSISTENT_ENFORCE_INTERVAL` | 5,000 | PERSISTENT エンフォース間隔 |
| `PERSISTENT_CLEAN_THRESHOLD` | 60,000 | PERSISTENT → STABLE 条件 |
| `ETW_BOOST_RATE_LIMIT` | 1,000 | ETW ブーストレートリミット |
| `SAFETY_NET_INTERVAL` | 10,000 | Safety Net 間隔 |
| `VIOLATION_THRESHOLD` | 3 | PERSISTENT 遷移閾値 |
| `STATS_LOG_INTERVAL` | 60,000 | 統計ログ間隔 |
| `JOB_QUERY_INTERVAL` | 5,000 | Job Object リフレッシュ間隔 |
| `ETW_HEALTH_CHECK_INTERVAL` | 30,000 | ETW ヘルスチェック間隔 |
| `DEGRADED_SCAN_INTERVAL` | 30,000 | 縮退スキャン間隔 |
| `CONFIG_DEBOUNCE_MS` | 2,000 | 設定変更デバウンス |
| `ERROR_LOG_SUPPRESS_MS` | 60,000 | エラーログ抑制ウィンドウ |
| `ETW_STABLE_RATE_LIMIT` | 200 | STABLE ETW レートリミット |
| `ECOQOS_CACHE_DURATION` | 100 | EcoQoS マイクロキャッシュ TTL |
| `LIVENESS_CHECK_INTERVAL` | 60,000 | ゾンビプロセス検出間隔 |
| `DIAG_LOG_INTERVAL_MS` | 30,000 (Debug) / 120,000 (Release) | [DIAG] ダンプ間隔 |
| `SUPPRESSION_CLEANUP_INTERVAL` | 60,000 | errorLogSuppression_ TTL クリーンアップ間隔 |
| `SUPPRESSION_TTL` | 300,000 | エラーログ抑制エントリの生存時間 (5 分) |
| `MEM_LOG_INTERVAL_SHORT` | 10,000 | [MEM] ログ間隔 (起動後 30 分以内) |
| `MEM_LOG_INTERVAL_LONG` | 60,000 | [MEM] ログ間隔 (起動後 30 分以降) |
| `MEM_LOG_WARMUP_MS` | 1,800,000 | [MEM] ウォームアップ期間 (30 分) |

容量制限定数 (`size_t` 型):

| 定数名 | 値 | 説明 |
|--------|-----|------|
| `SUPPRESSION_MAX_SIZE` | 2,000 | errorLogSuppression_ 緊急キャップ (oldest-first eviction) |
| `POLICY_SET_MAX_SIZE` | 1,000 | policyAppliedSet_ 緊急キャップ (swap クリア) |

### A.8 レジストリ定数 (registry_manager.h)

| 定数名 | 値 |
|--------|----|
| `REG_POWER_THROTTLING_PATH` | `SYSTEM\CurrentControlSet\Control\Power\PowerThrottling` |
| `REG_IFEO_PATH` | `SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options` |
| `IFEO_CPU_PRIORITY_HIGH` | 3 |
| `POLICY_MANIFEST_FILENAME` | `L"UnLeaf_policies.ini"` |

### A.9 enum 値

#### ProcessPhase (engine_core.h)

| 値 | 意味 |
|----|------|
| `AGGRESSIVE` | 起動時: One-shot SET + 遅延検証 (3s) |
| `STABLE` | 定常状態: イベント駆動のみ |
| `PERSISTENT` | 頑固 EcoQoS: 5s 間隔 SET |

#### EnforcementRequestType (engine_core.h)

| 値 | 意味 |
|----|------|
| `ETW_PROCESS_START` | ETW でプロセス起動検知 |
| `ETW_THREAD_START` | ETW でスレッド生成検知 |
| `DEFERRED_VERIFICATION` | タイマーベース遅延検証 |
| `PERSISTENT_ENFORCE` | PERSISTENT 周期エンフォース |
| `SAFETY_NET` | 保険チェック |

#### WaitIndex (engine_core.h)

| 値 | インデックス | 意味 |
|----|-------------|------|
| `WAIT_STOP` | 0 | サービス停止シグナル |
| `WAIT_CONFIG_CHANGE` | 1 | 設定変更通知 |
| `WAIT_SAFETY_NET` | 2 | Safety Net タイマー |
| `WAIT_ENFORCEMENT_REQUEST` | 3 | キューイベント |
| `WAIT_PROCESS_EXIT` | 4 | プロセス終了通知 |
| `WAIT_COUNT` | 5 | ハンドル総数 |

#### OperationMode (engine_core.h)

| 値 | 意味 |
|----|------|
| `NORMAL` | ETW + Safety Net (フル機能) |
| `DEGRADED_ETW` | Toolhelp32 フォールバック |
| `DEGRADED_CONFIG` | 設定読み込み失敗 |

#### LogLevel (types.h)

| 値 | 数値 | 意味 |
|----|------|------|
| `LOG_ERROR` | 0 | 常に出力 |
| `LOG_ALERT` | 1 | 警告以上 |
| `LOG_INFO` | 2 | 通常 (デフォルト) |
| `LOG_DEBUG` | 3 | 開発用 |

#### IPCCommand (types.h)

| 値 | 数値 | 権限 |
|----|------|------|
| `CMD_ADD_TARGET` | 1 | ADMIN |
| `CMD_REMOVE_TARGET` | 2 | ADMIN |
| `CMD_GET_STATUS` | 3 | PUBLIC |
| `CMD_STOP_SERVICE` | 4 | SYSTEM_ONLY |
| `CMD_GET_CONFIG` | 5 | PUBLIC |
| `CMD_SET_INTERVAL` | 6 | ADMIN |
| `CMD_GET_LOGS` | 7 | PUBLIC |
| `CMD_GET_STATS` | 8 | PUBLIC |
| `CMD_HEALTH_CHECK` | 9 | PUBLIC |
| `CMD_SET_LOG_ENABLED` | 10 | ADMIN |

#### IPCResponse (types.h)

| 値 | 数値 |
|----|------|
| `RESP_SUCCESS` | 0 |
| `RESP_ERROR_GENERAL` | 1 |
| `RESP_ERROR_NOT_FOUND` | 2 |
| `RESP_ERROR_ACCESS_DENIED` | 3 |
| `RESP_ERROR_INVALID_INPUT` | 4 |
| `RESP_STATUS_UPDATE` | 10 |
| `RESP_LOG_STREAM` | 11 |

#### CommandPermission (security.h)

| 値 | 意味 |
|----|------|
| `PUBLIC` | 接続済みクライアントは全員アクセス可 |
| `ADMIN` | Administrators グループ必須 |
| `SYSTEM_ONLY` | SYSTEM または昇格済み Administrator |

#### AuthResult (security.h)

| 値 | 意味 |
|----|------|
| `AUTHORIZED` | 認可済み |
| `UNAUTHORIZED` | 権限不足 |
| `ERROR_IMPERSONATION` | 偽装失敗 |
| `ERROR_TOKEN` | トークン取得失敗 |

---

## B. IPC コマンド一覧

| コマンド | ID | 権限 | リクエストデータ | レスポンスデータ |
|---------|-----|------|----------------|---------------|
| ADD_TARGET | 1 | ADMIN | UTF-8 プロセス名 | `{"success": true}` |
| REMOVE_TARGET | 2 | ADMIN | UTF-8 プロセス名 | `{"success": true}` |
| GET_STATUS | 3 | PUBLIC | なし | `{"running": true, "version": "1.00"}` |
| STOP_SERVICE | 4 | SYSTEM_ONLY | なし | `{"result": "stopping"}` |
| GET_CONFIG | 5 | PUBLIC | なし | (ハンドラ依存) |
| SET_INTERVAL | 6 | ADMIN | uint32_t (4 bytes) | (ハンドラ依存) |
| GET_LOGS | 7 | PUBLIC | LogRequest (8 bytes) | LogResponseHeader + data |
| GET_STATS | 8 | PUBLIC | なし | uint32_t (追跡プロセス数) |
| HEALTH_CHECK | 9 | PUBLIC | なし | JSON (詳細ヘルス情報) |
| SET_LOG_ENABLED | 10 | ADMIN | 1 byte (0/1) | `{"success": true}` |

### ログ取得プロトコル

```
リクエスト:
  LogRequest { uint64_t offset }  // クライアントの現在読み取り位置

レスポンス:
  LogResponseHeader {
    uint64_t newOffset;    // 次回の開始位置
    uint32_t dataLength;   // 後続ログデータ長
  }
  + [dataLength バイトのログデータ]
```

- ログローテーション検知: `clientOffset > currentSize` → offset をリセット
- 差分読み取り: 毎回 `UNLEAF_MAX_LOG_READ_SIZE` (8KB) まで返却
- ファイル共有: `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` で Logger ローテーション (MoveFileExW) を妨げない

### HEALTH_CHECK レスポンス構造

```json
{
  "schema_version": 1,
  "status": "healthy|degraded|unhealthy",
  "uptime_seconds": 3600,
  "engine": {
    "running": true,
    "mode": "NORMAL",
    "active_processes": [
      {"pid": 1234, "name": "chrome.exe", "phase": "STABLE", "violations": 0, "is_child": false}
    ],
    "total_violations": 12,
    "phases": { "aggressive": 1, "stable": 3, "persistent": 1 }
  },
  "etw": { "healthy": true, "event_count": 45000 },
  "wakeups": {
    "config_change": 2, "safety_net": 360,
    "enforcement_request": 150, "process_exit": 10
  },
  "enforcement": {
    "persistent_applied": 5, "persistent_skipped": 200,
    "total": 500, "success": 495, "fail": 5,
    "avg_latency_us": 120, "max_latency_us": 5000,
    "etw_thread_deduped": 42,
    "last_enforce_time_ms": 1740000000000
  },
  "errors": { "access_denied": 0, "invalid_parameter": 3, "shutdown_warnings": 0 },
  "config": { "changes_detected": 2, "reloads": 1 },
  "ipc": { "healthy": true }
}
```

---

## C. レジストリキー一覧

### C.1 PowerThrottling (EcoQoS 永続除外)

| 項目 | 値 |
|------|-----|
| ルート | `HKEY_LOCAL_MACHINE` |
| パス | `SYSTEM\CurrentControlSet\Control\Power\PowerThrottling` |
| 値名 | exe のフルパス (例: `C:\Program Files\App\app.exe`) |
| 値型 | `REG_DWORD` |
| 値 | `1` |
| 作成 | `ApplyPolicy()` → `DisablePowerThrottling()` |
| 削除 | `CleanupAllPolicies()` / `RemoveAllPolicies()` → `EnablePowerThrottling()` |

### C.2 IFEO PerfOptions (プロセス優先度)

| 項目 | 値 |
|------|-----|
| ルート | `HKEY_LOCAL_MACHINE` |
| パス | `SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<exe名>\PerfOptions` |
| 値名 | `CpuPriorityClass` |
| 値型 | `REG_DWORD` |
| 値 | `3` (HIGH) |
| 作成 | `ApplyPolicy()` → `SetIFEOPerfOptions()` |
| 削除 | `CleanupAllPolicies()` / `RemoveAllPolicies()` → `RemoveIFEOPerfOptions()` |

IFEO PerfOptions の削除時は、`PerfOptions` サブキーを削除した後、親の `<exe名>` キーが空なら同様に削除を試みる (他の IFEO 設定が無い場合のクリーンアップ)。

---

## D. クリティカルプロセス保護リスト

以下の 18 プロセスは、ターゲットとしての登録が禁止されている。`IsCriticalProcess()` で照合する。

| # | プロセス名 | 役割 |
|---|-----------|------|
| 1 | `ntoskrnl.exe` | Windows カーネル |
| 2 | `smss.exe` | Session Manager |
| 3 | `csrss.exe` | Client/Server Runtime |
| 4 | `wininit.exe` | Windows 初期化 |
| 5 | `services.exe` | Service Control Manager |
| 6 | `lsass.exe` | Local Security Authority |
| 7 | `winlogon.exe` | ログオンプロセス |
| 8 | `svchost.exe` | 汎用サービスホスト |
| 9 | `explorer.exe` | Windows シェル |
| 10 | `dwm.exe` | Desktop Window Manager |
| 11 | `ctfmon.exe` | テキスト入力サービス |
| 12 | `unleaf_service.exe` | UnLeaf サービス (自身) |
| 13 | `unleaf_manager.exe` | UnLeaf マネージャー (自身) |
| 14 | `fontdrvhost.exe` | フォントドライバホスト |
| 15 | `audiodg.exe` | オーディオデバイスグラフ |
| 16 | `conhost.exe` | コンソールホスト |
| 17 | `securityhealthservice.exe` | Windows セキュリティ |
| 18 | `msmpeng.exe` | Windows Defender |

照合は小文字変換後に行う (`ToLower()`)。

---

## E. 将来予定 (v1.0.2+)

以下は v1.0.1 には含まれない将来の改善予定項目。

| 項目 | 優先度 | 内容 |
|------|--------|------|
| engine_core コールバック層テスト | 中 | `DeferredVerifyTimerCallback` / `PersistentEnforceTimerCallback` 等のユニットテスト追加 |
| Manager ロギング統一 | 中 | `AppendLog` を共通ロガーに統合 (UI スレッド同期設計が必要) |
| GdiplusShutdown 追加 | 低 | プロセス終了時の明示的 GDI+ シャットダウン (現状は OS 回収) |
| 設定保存 (ウィンドウ位置・状態永続化) | 低 | `UnLeafConfig` に `[Manager]` セクション追加 |
| ThemePalette 構造体への色定数集約 | 低 | `clr*_` メンバを構造化し保守性向上 |
| WDAC ポリシー対応 | 中 | テストバイナリの署名要件対応 |

---

## F. 変更履歴 (Changelog)

### v1.0.1 (2026-03-09)

**バグ修正・安全性強化**

| § | 内容 |
|---|------|
| §8.37 | `ToggleSubclassProc` に null チェックを追加 (Manager クラッシュ防止) |
| §8.38 | `DeleteTimerQueueTimer` を `trackedCs_` 取得前に移動し、ロック保持中の呼び出しを完全排除 (全5箇所) |
| §8.39 | `DrawButton` の `SelectObject` 旧フォント復元を追加 (GDI オブジェクトリーク修正) |

**アーキテクチャ改善 (テスト可能性向上)**

| § | 内容 |
|---|------|
| §8.40 | `src/engine/` を新設。`IsTargetProcess` / `IsCacheValid` / `ShouldExitPersistent` 等 5 関数を Win32 非依存の純粋 C++ モジュール `engine_logic` に抽出。`ProcessPhase` を type alias 化 |
| §8.41 | `tests/test_engine_logic.cpp` を新規作成。32 テストケース、102/102 PASS |
| §8.42 | `src/engine/engine_policy.h` を新規作成。`EnginePolicy` 構造体でタイミング定数を一元管理。`IsCacheValid` / `ShouldExitPersistent` のシグネチャを `const EnginePolicy&` 渡しに統一。`EngineCore` に `policy_` メンバを追加。`tests/test_engine_policy.cpp` 新規作成。104/104 PASS |
| §8.43 | `engine_logic.cpp` の `default case` コメントを明確化 (C++17 制約 / noexcept 適用状況を記述) |

**ドキュメント**

| § | 内容 |
|---|------|
| §8.44 | `Engine_Specification.md` を更新 (§4.5 EnginePolicy 構造体、§13.5 engine_logic 純粋ロジックモジュールを新設) |

**CI/CD 基盤**

| § | 内容 |
|---|------|
| §8.45 | `.github/workflows/build.yml` を新規作成。`push` / `pull_request` 時に `windows-latest` で configure → build → ctest を自動実行 |
| §8.46 | `docs/GitHub_CI_Operation.md` を新規作成 (CI 運用手順書) |

**パッチ (リソース管理・CI)**

| § | 内容 |
|---|------|
| §8.47 | `DrawToggleSwitch()` の GDI+ 初期化を `GdipGuard` RAII 構造体に置き換え。プロセス終了時の static 破棄フェーズで `GdiplusShutdown` を確実に呼び出す。動作変更なし |
| §8.47 | CI キャッシュパスを `build/` → `build/_deps` に変更。`hashFiles` を `**/CMakeLists.txt` グロブに変更。FetchContent 依存 (GoogleTest / nlohmann_json) のみをキャッシュ対象とし cache hit 率を向上 |
| §8.48 | `engine_core.cpp` の Windows バージョン表示名修正: major=10 かつ build >= 22000 → `Windows 11`、major=10 かつ build < 22000 → `Windows 10`、major >= 11 → `Windows {major}.{minor}` のハイブリッド方式。Windows 11 環境で起動ログが `Windows 10.0` と誤表示されていた問題を修正。`WINDOWS_11_BUILD_THRESHOLD` を間接的に再利用 (`isWindows11OrLater` フラグ経由) |

> **Version Notes (v1.0.1 patch)**: 本パッチは主に Manager 側のリソースライフサイクル改善と CI 最適化を含む。加えて §8.48 にて Engine 側の Windows バージョン表示名ログ修正を実施 (動作ロジックの変更なし)。

### v1.0.0 (2026-03-06)

初回リリース。イベント駆動アーキテクチャ、3フェーズ適応制御、5層防御、IPC、RegistryPolicyManager、Manager UI (ダークテーマ固定)、72 ユニットテスト PASS。長期計測 (42.74h / 29.62h) による安定性確認。

---

> **End of Document**
