# UnLeaf Engine 開発者仕様書

> **対象バージョン**: v1.00 (C++ Native)
> **最終更新**: 2026-02-15
> **対象読者**: 本プロジェクトの保守・拡張を行う開発者

---

## 目次

- [第1部: 概要設計 (Overview Design)](#第1部-概要設計-overview-design)
  - [1. システム概要](#1-システム概要)
  - [2. アーキテクチャ全体像](#2-アーキテクチャ全体像)
  - [3. EcoQoS 解除の全体処理フロー](#3-ecoqos-解除の全体処理フロー)
- [第2部: 基本設計 (Basic Design)](#第2部-基本設計-basic-design)
  - [4. EngineCore 基本設計](#4-enginecore-基本設計)
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

- **EngineControlThread**: `EngineControlLoop()` を実行する単一スレッド。WFMO で 5 つのハンドルを待機し、全ての状態変更をこのスレッド上でシリアルに処理する。
- **ETW ConsumerThread**: `ProcessTrace()` のブロッキングコールを実行する。イベント受信時にコールバックが呼ばれ、`EnqueueRequest()` 経由で EngineControlThread にリクエストを送る。
- **IPC ServerThread**: Named Pipe の接続待機とコマンド処理を行う。
- **OS Thread Pool**: `CreateTimerQueueTimer` や `RegisterWaitForSingleObject` のコールバックが実行される。これらは `EnqueueRequest()` や `pendingRemovalPids_` 経由で EngineControlThread にイベントを送る。

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
  - Step 2: 1,000ms 後にチェック
  - Step 3: 3,000ms 後にチェック (最終検証)
- **遷移条件**:
  - 全ステップ clean → **STABLE**
  - 検証中に EcoQoS ON → `violationCount++`, PulseEnforceV6 再実行, 検証シーケンスリセット
  - `violationCount >= 3` → **PERSISTENT**

#### STABLE フェーズ

- **トリガー**: AGGRESSIVE の 3 回検証パス、または PERSISTENT の 60s clean
- **動作**: アクティブなポーリングやタイマーなし。以下のイベントでのみ処理:
  - ETW Thread Start → `IsEcoQoSEnabled` チェック
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

```
図4: EcoQoS 解除 5層防御フロー

  PulseEnforceV6(hProcess, pid, isIntensive)
  │
  │  Layer 1: Background Mode Exit (無条件)
  │  ├── SetPriorityClass(hProcess, PROCESS_MODE_BACKGROUND_END)
  │
  │  Layer 2: NtSetInformationProcess (Windows 11+ のみ)
  │  ├── ntdll!NtSetInformationProcess
  │  │   ProcessInformationClass = 77 (NT_PROCESS_POWER_THROTTLING_STATE)
  │  │   ControlMask = EXECUTION_SPEED | IGNORE_TIMER (0x5)
  │  │   StateMask = 0 (Force OFF)
  │  │
  │  │   成功 → Layer 3 スキップ
  │  │   失敗 → Layer 3 へフォールバック
  │
  │  Layer 3: SetProcessInformation (Win10 primary / Win11 fallback)
  │  ├── SetProcessInformation(ProcessPowerThrottling)
  │  │   ControlMask = バージョン依存 (Win11: 0x5, Win10: 0x1)
  │  │   StateMask = 0 (Force OFF)
  │
  │  Layer 4: Priority Class (無条件)
  │  ├── SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS)
  │  │   OS の自動 EcoQoS 適用を防止
  │
  │  Layer 5: Thread Throttling (isIntensive == true のみ)
  │  └── DisableThreadThrottling(pid, aggressive=true)
  │       CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)
  │       各スレッド:
  │         SetThreadInformation(ThreadPowerThrottling, StateMask=0)
  │         GetThreadPriority → ABOVE_NORMAL に昇格 (< ABOVE_NORMAL の場合)
```

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

### 6.3 Windows バージョン分岐

| 機能 | Windows 11 (Build >= 22000) | Windows 10 |
|------|---------------------------|------------|
| NtSetInformationProcess | 使用 (Layer 2) | 不使用 |
| IGNORE_TIMER フラグ (0x4) | ControlMask に含む | 含まない (エラー回避) |
| SetProcessInformation | フォールバック (Layer 3) | プライマリ |
| NtQueryInformationProcess | IsEcoQoSEnabled で優先使用 | フォールバックのみ |

バージョン検出は `Initialize()` 時に `RtlGetVersion` で行う。`winVersion_.isWindows11OrLater` で Build 22000 以上を判定する。

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

### 7.4 静的インスタンスポインタ

ETW コールバックは C API であり、`this` ポインタを受け取れない。`ProcessMonitor::instance_` 静的メンバで唯一のインスタンスへのルーティングを行う。`Start()` で設定し、`Stop()` で `nullptr` にクリアする。

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

### 8.3 コマンド処理

サーバーループは Overlapped I/O を使用して停止シグナルとの同時待機を実現する:

1. `CreateNamedPipeW()` でパイプインスタンス作成
2. `ConnectNamedPipe()` + Overlapped で接続待機
3. `WaitForMultipleObjects(overlapped.hEvent, stopEvent_)` で待機
4. クライアント接続時: `HandleClient()` でコマンド処理
5. コマンド処理後: パイプをクローズし、次のインスタンスを作成

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

### 9.4 JSON マイグレーション

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

| レベル | 値 | ラベル | 用途 |
|--------|-----|--------|------|
| ERROR | 0 | `ERR ` | 常に出力。致命的エラー |
| ALERT | 1 | `ALRT` | 警告。異常だが回復可能 |
| INFO | 2 | `INFO` | 通常運用メッセージ (デフォルト) |
| DEBUG | 3 | `DEBG` | 開発詳細。フェーズ遷移、エンフォース結果 |

出力判定: `static_cast<uint8_t>(level) <= static_cast<uint8_t>(currentLevel_)`

### 11.2 ローテーション

| 項目 | 値 |
|------|-----|
| ファイル名 | `UnLeaf.log` |
| バックアップ名 | `UnLeaf.log.1` |
| 最大サイズ | 100 KB (`MAX_LOG_SIZE = 102400`) |
| ローテーション方式 | 閉じて → バックアップ削除 → リネーム → 新規作成 |
| ファイルオープンモード | `FILE_APPEND_DATA \| FILE_SHARE_READ \| FILE_SHARE_WRITE` |
| エンコーディング | UTF-8 |

ローテーションは `WriteMessage()` 内で `CheckRotation()` を呼び、書き込み前にファイルサイズを確認する。

### 11.3 動的制御

- **ログレベル変更**: INI の `LogLevel` 値を変更 → 設定リロードで即時反映
- **ログ有効/無効**: INI の `LogEnabled` またはIPCの `CMD_SET_LOG_ENABLED` で切替
- **`enabled_`**: `std::atomic<bool>` で保護。acquire/release セマンティクスで安全にアクセス

### 11.4 出力フォーマット

```
[YYYY/MM/DD HH:MM:SS] [LEVL] メッセージ
```

例:
```
[2026/02/15 14:30:00] [INFO] EngineCore started: 3 targets, NORMAL mode, Event-Driven, SafetyNet=10s
[2026/02/15 14:30:01] [DEBG] Optimized: [TARGET] chrome.exe (PID: 1234) Child=0
```

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
  │       └── Stats log (60s): phase breakdown 出力
  │
  └── 最終ドレイン: ProcessPendingRemovals()
```

### 12.4 Stop() 9ステップ シャットダウンシーケンス

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
| `cs_` | LightweightLogger | `fileHandle_`, `initialized_` 等 |
| `policyCs_` | RegistryPolicyManager | `appliedPolicies_`, `manifestPath_` |

### 14.2 ロック順序

デッドロックを回避するため、複数のロックを取得する場合は以下の順序を守る:

```
jobCs_ → trackedCs_ (RefreshJobObjectPids で使用)
```

その他のロックは同時取得されないか、単独で使用される。

### 14.3 atomic パターン

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
| `configChangeDetected_` | `atomic<uint32_t>` | 設定変更検知数 |
| `configReloadCount_` | `atomic<uint32_t>` | 設定リロード数 |
| `enabled_` (Logger) | `atomic<bool>` | ログ出力有効/無効 (acquire/release) |

統計カウンタは `memory_order_relaxed` で十分 (厳密な順序は不要)。
`stopRequested_` は `memory_order_seq_cst` (デフォルト) で、停止シグナルの可視性を保証する。

---

## 15. エラー処理・自己修復

### 15.1 HandleEnforceError

```
HandleEnforceError(hProcess, pid, error)
  │
  ├── totalRetries_++
  ├── エラーコード別カウンタ更新 (error5Count_ / error87Count_)
  │
  ├── プロセス生存確認: GetExitCodeProcess()
  │   → 終了済み → processHandle.reset() (即クリーンアップ)
  │
  ├── エラーログ抑制チェック
  │   errorLogSuppression_[(pid, error)] → 60s 以内なら抑制
  │
  └── switch (error)
      │
      ├── ERROR_ACCESS_DENIED (5)
      │   consecutiveFailures <= 2 → リトライ (50ms backoff)
      │   consecutiveFailures > 2  → 諦め (ログ出力のみ)
      │
      ├── ERROR_INVALID_HANDLE
      │   → processHandle.reset() (ハンドル無効化)
      │
      ├── ERROR_INVALID_PARAMETER (87)
      │   → processHandle.reset() (プロセス終了と見なす)
      │
      └── その他
          consecutiveFailures <= MAX_RETRY_COUNT (5)
          → 指数バックオフ (50ms × 2^n)
          consecutiveFailures > 5
          → 諦め (ログ出力のみ)
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

`CreateAndAssignJobObject()` は以下の条件でのみ Job Object を作成する:

1. ルートターゲットプロセス (`isChild == false`)
2. プロセスが既に Job Object に属していない (`IsProcessInJob()` → `FALSE`)
   - Chrome のサンドボックスプロセスなどは既に Job に属しているため、作成をスキップ
   - この場合でも `TrackedProcess` は作成され、PulseEnforce は継続

Job Object 設定:
```
JOBOBJECT_EXTENDED_LIMIT_INFORMATION:
  LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK
```

`BREAKAWAY_OK` により、子プロセスが Job から離脱することを許可する。

### 16.2 RefreshJobObjectPids 2-pass アルゴリズム

```
RefreshJobObjectPids()
  │
  │  ──── Pass 1: ロック保持 (データ収集) ────
  │
  ├── CSLockGuard(jobCs_)
  ├── CSLockGuard(trackedCs_)   ← ロック順序: jobCs_ → trackedCs_
  │
  ├── 各 JobObjectInfo:
  │   ├── QueryInformationJobObject(JobObjectBasicProcessIdList)
  │   │   スタック上の固定長バッファ (MAX_JOB_PIDS = 1024)
  │   ├── PID リストから未追跡の PID を収集
  │   └── JobQueryResult { rootPid, newPids[] }
  │
  │  ──── Pass 2: ロック解放 (処理実行) ────
  │
  └── 各 newPid:
      ├── IsTracked(pid) → true → skip (二重チェック)
      ├── OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
      ├── QueryFullProcessImageNameW → ファイル名抽出
      ├── IsCriticalProcess → skip
      └── ApplyOptimization(pid, name, isChild=true, rootPid)
```

2-pass の目的: ロック保持時間の最小化。Pass 2 の `OpenProcess` / `ApplyOptimization` はカーネル呼び出しを含むため、ロック外で実行する。

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
| GET_STATUS | 3 | PUBLIC | なし | `{"running": true, "version": "2.00"}` |
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
- ファイル共有: `FILE_SHARE_READ | FILE_SHARE_WRITE` で Logger と競合しない

### HEALTH_CHECK レスポンス構造

```json
{
  "status": "healthy|degraded|unhealthy",
  "uptime_seconds": 3600,
  "engine": {
    "running": true,
    "mode": "NORMAL",
    "active_processes": 5,
    "total_violations": 12,
    "phases": { "aggressive": 1, "stable": 3, "persistent": 1 }
  },
  "etw": { "healthy": true, "event_count": 45000 },
  "wakeups": {
    "config_change": 2, "safety_net": 360,
    "enforcement_request": 150, "process_exit": 10
  },
  "enforcement": { "persistent_applied": 5, "persistent_skipped": 200 },
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

> **End of Document**
