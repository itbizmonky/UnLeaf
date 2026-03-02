# UnLeaf v1.00 Project Context (Check-point)

## 1. 開発環境 (Environment)
- **Target OS**: Windows 11
- **IDE/Compiler**: Visual Studio 2026 (Internal Version 18)
- **CMake Generator**: `Visual Studio 18 2026`
- **Architecture**: x64 / Release
- **Build Command**: `cmake --build "D:\Desktop\chrome_addon\UnLeaf_v1.00\build" --config Release`

## 2. プロジェクトの現状 (Current Status)

### 最新バージョン: v1.00 (イベント駆動アーキテクチャ)

#### v1.00 修正内容 (ARCHITECTURE.md 準拠リファクタリング)

**設計思想の変更:**
- **ポーリング駆動 → イベント駆動** への完全移行
- ARCHITECTURE.md の「ポーリング禁止」原則を実装レベルで遵守
- 監視の主軸を OS イベント（プロセス生成・スレッド生成）に変更
- Safety Net は「保険的整合性確認」であり、監視手段ではない

| 項目 | v7.93 (旧) | v1.00 (新) |
|------|-----------|-----------|
| メインループ | 10ms ポーリング | WaitForMultipleObjects(INFINITE) |
| AGGRESSIVE 期間 | 10ms SET × 300回 | 1回 SET + 遅延検証 3回 |
| STABLE 期間 | 500ms GET ループ | イベント駆動のみ |
| PERSISTENT 期間 | 50ms SET (20回/秒) | 5秒 SET + ETW ブースト (即時応答) |
| 設定変更検知 | 5秒ポーリング | OS ファイル通知 |
| スレッド数 (Service) | 4 | 3 |
| アイドル時 CPU | 常時ウェイクアップ | ゼロ |
| ウェイクアップ頻度 | ~6000回/分 | ~6回/分 |

#### 修正ファイル (v1.00)

| ファイル | 変更内容 |
|----------|----------|
| `src/common/types.h` | VERSION 1.00、旧ポーリング定数削除 |
| `src/service/process_monitor.h` | ThreadStartCallback 追加、Start() シグネチャ変更 |
| `src/service/process_monitor.cpp` | ETW keyword 0x10→0x30、スレッドイベント処理追加 |
| `src/service/engine_core.h` | 完全再設計: イベント駆動アーキテクチャ |
| `src/service/engine_core.cpp` | EnforcementLoop削除、EngineControlLoop新規実装 |

#### 削除された機能
- `EnforcementLoop()` — 10ms ポーリングループ
- `ConfigWatcherLoop()` — 5秒設定監視ループ
- `QuickRescan()` — 500ms プロセススキャン
- `timeBeginPeriod(1)` / `timeEndPeriod(1)` — 高精度タイマー
- `previousScanPids_` — 差分検出用バッファ
- `enforcementThread_`, `configWatcherThread_` — 個別スレッド

#### 新規追加された機能

**イベント駆動制御ループ (Phase 1 適用後):**
```cpp
void EngineCore::EngineControlLoop() {
    HANDLE waitHandles[WAIT_COUNT];  // WAIT_COUNT = 5
    waitHandles[WAIT_STOP] = stopEvent_;
    waitHandles[WAIT_CONFIG_CHANGE] = configChangeHandle_;
    waitHandles[WAIT_SAFETY_NET] = safetyNetTimer_;
    waitHandles[WAIT_ENFORCEMENT_REQUEST] = enforcementRequestEvent_;
    waitHandles[WAIT_PROCESS_EXIT] = hWakeupEvent_;

    while (!stopRequested_.load()) {
        DWORD waitResult = WaitForMultipleObjects(WAIT_COUNT, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + WAIT_STOP) break;      // Pattern B: WFMO結果明示判定
        if (waitResult == WAIT_FAILED) { LOG_ERROR(...); break; } // early check
        switch (waitResult) { /* CONFIG_CHANGE, SAFETY_NET, ENFORCEMENT_REQUEST, PROCESS_EXIT */ }
        PerformPeriodicMaintenance(now);
    }
    ProcessPendingRemovals();  // final drain
}
```

**ETW スレッドイベント:**
```cpp
// keyword 0x30 = PROCESS (0x10) | THREAD (0x20)
EnableTraceEx2(..., keyword=0x30, ...);

// スレッド生成 = EcoQoS 再設定の主要契機
if (eventId == EVENT_ID_THREAD_START) {
    instance_->threadCallback_(threadId, ownerPid);
}
```

**遅延検証システム (Timer Queue):**
```cpp
// AGGRESSIVE フェーズ: One-Shot + Deferred Verification
t=0ms     : PulseEnforceV6() — 即時制御
t=200ms   : IsEcoQoSEnabled() — 遅延検証 #1
t=1000ms  : IsEcoQoSEnabled() — 遅延検証 #2
t=3000ms  : IsEcoQoSEnabled() — 最終検証 → STABLE 移行判定
```

**Safety Net (保険的整合性確認):**
```cpp
// 10秒間隔の Waitable Timer
// 監視手段ではなく、イベント取りこぼし補償
safetyNetTimer_ = CreateWaitableTimer(nullptr, FALSE, nullptr);
SetWaitableTimer(safetyNetTimer_, &dueTime, 10000, ...);
```

**ETW ブースト (PERSISTENT フェーズ即時応答):**
```cpp
// OnThreadStart(): PERSISTENT フェーズでも ETW スレッドイベントをキューイング
if (currentPhase == ProcessPhase::STABLE || currentPhase == ProcessPhase::PERSISTENT) {
    EnqueueRequest(EnforcementRequest(ownerPid, EnforcementRequestType::ETW_THREAD_START));
}

// ProcessEnforcementQueue(): 同一PID ETW_THREAD_START 重複排除
// → バースト時の O(N^2) スナップショット生成を O(N) に削減

// DispatchEnforcementRequest():
// STABLE: 200ms レートリミット + IsEcoQoSEnabledCached (100ms TTL)
// PERSISTENT: 1000ms レートリミット + IsEcoQoSEnabledCached (100ms TTL)
```

**設定変更通知 (FindFirstChangeNotification):**
```cpp
configChangeHandle_ = FindFirstChangeNotificationW(
    baseDir_.c_str(),
    FALSE,
    FILE_NOTIFY_CHANGE_LAST_WRITE
);
```

## 3. 新タイミング定数 (v1.00)

```cpp
// engine_core.h
static constexpr ULONGLONG SAFETY_NET_INTERVAL = 10000;       // 10秒
static constexpr ULONGLONG DEFERRED_VERIFY_1 = 200;           // 200ms
static constexpr ULONGLONG DEFERRED_VERIFY_2 = 1000;          // 1秒
static constexpr ULONGLONG DEFERRED_VERIFY_FINAL = 3000;      // 3秒
static constexpr ULONGLONG PERSISTENT_ENFORCE_INTERVAL = 5000; // 5秒
static constexpr ULONGLONG PERSISTENT_CLEAN_THRESHOLD = 60000; // 60秒クリーンで STABLE 復帰
static constexpr ULONGLONG ETW_BOOST_RATE_LIMIT = 1000;        // ETW ブースト レート制限 (1秒)
static constexpr ULONGLONG ETW_STABLE_RATE_LIMIT = 200;        // STABLE フェーズ ETW レート制限 (200ms)
static constexpr ULONGLONG ECOQOS_CACHE_DURATION = 100;        // EcoQoS 状態キャッシュ TTL (100ms)
static constexpr ULONGLONG MAINTENANCE_INTERVAL = 60000;       // 60秒
```

## 4. スレッド構成 (v1.00)

| # | スレッド | 役割 | ブロッキング方式 |
|---|----------|------|-----------------|
| 1 | engineControlThread_ | 制御ループ統合 | WaitForMultipleObjects(5 handles, INFINITE) |
| 2 | consumerThread_ | ETW イベント消費 | ProcessTrace() |
| 3 | serverThread_ | IPC Named Pipe | ConnectNamedPipe() |

## 5. コマンド権限マトリクス

| コマンド | 権限レベル | 説明 |
|----------|-----------|------|
| CMD_GET_STATUS | PUBLIC | ステータス取得 |
| CMD_GET_LOGS | PUBLIC | ログ取得 |
| CMD_GET_STATS | PUBLIC | 統計取得 |
| CMD_GET_CONFIG | PUBLIC | 設定取得 |
| CMD_HEALTH_CHECK | PUBLIC | ヘルスチェック |
| CMD_ADD_TARGET | ADMIN | ターゲット追加 |
| CMD_REMOVE_TARGET | ADMIN | ターゲット削除 |
| CMD_SET_INTERVAL | ADMIN | インターバル設定 |
| CMD_SET_LOG_ENABLED | ADMIN | ログON/OFF設定 |
| CMD_STOP_SERVICE | SYSTEM_ONLY | サービス停止 |

## 6. ビルド出力 (Build Output)
```
D:\Desktop\chrome_addon\UnLeaf_v1.00\build\Release\
├── UnLeaf_Manager.exe
├── UnLeaf_Service.exe
├── UnLeaf_Tests.exe    (テスト専用・製品版に含めない / UNLEAF_BUILD_TESTS=ON 時のみ生成)
├── UnLeaf.ini
└── UnLeaf.log
```

## 7. 検証項目 (v1.00)

- [x] ビルド成功 (2026/02/06)
- [x] ETW スレッドイベント受信確認 (2026/02/07)
- [x] AGGRESSIVE フェーズ: 即時 enforce + 遅延検証ログ確認 (2026/02/07)
- [x] STABLE フェーズ: アイドル時 CPU 0% 確認 (2026/02/07)
- [x] Safety Net: 10秒毎ログ確認 (2026/02/07)
- [x] 設定変更: INI 編集 → 即座にリロード確認 (2026/02/07)
- [x] スレッド数: Process Explorer で 3 スレッド確認 (2026/02/07 — ログ上正常、実機はユーザー確認)
- [x] PERSISTENT フェーズ: 5秒間隔 enforce 確認 (2026/02/07)
- [x] ETW ブースト: PERSISTENT タブ切替時の即時応答確認 (2026/02/07)

### 7.1 ETW ブースト — 静的検証 (2026/02/06 完了)

全コードパスをトレースし、以下を検証済み:

| 検証項目 | 結果 |
|---------|------|
| `lastEtwEnforceTime` フィールド初期化 (`0`) | OK — 初回チェック確実にパス |
| レート制限ロジック (1回/秒) | OK — `ETW_BOOST_RATE_LIMIT = 1000` |
| EcoQoS OFF 時の省電力性 | OK — `IsEcoQoSEnabled()` のみ、enforce なし |
| PERSISTENT→STABLE 復帰への影響 | OK — `lastViolationTime` 更新は違反時のみ |
| 5秒タイマーとの競合 | OK — キューでシリアライズ、冪等操作 |
| フェーズ遷移中のエッジケース | OK — STABLE フォールスルーで安全 |
| スレッドセーフティ | OK — 全操作が `trackedCs_` ロック下 |
| 統計の一貫性 (`violationCount`/`totalViolations_` 非加算) | OK — PERSISTENT_ENFORCE と同方針 |
| Release ビルド | OK — コンパイル・リンク成功 |

**変更ファイル:**
| ファイル | 行 | 変更内容 |
|----------|-----|----------|
| `engine_core.h` | L158 | `ULONGLONG lastEtwEnforceTime` フィールド追加 |
| `engine_core.h` | L171 | コンストラクタ初期化 `lastEtwEnforceTime(0)` |
| `engine_core.h` | L418 | `ETW_BOOST_RATE_LIMIT = 1000` 定数追加 |
| `engine_core.cpp` | L377 | `OnThreadStart()` に PERSISTENT 条件追加 |
| `engine_core.cpp` | L512-524 | `DispatchEnforcementRequest()` PERSISTENT ETW ブースト分岐追加 |

## 8. Phase 1: スレッド増殖の完全排除 — 完了 (2026/02/13)

**問題**: `OnProcessExit` コールバックが `deferredRemovalQueue_` に PID を push していたが、
ヘッダに宣言がなく、キューをドレインするコードも存在しなかった。
結果として、終了したプロセスが `trackedProcesses_` から除去されず、スレッドカウントが単調増加していた。

**解決**: pending removal queue + wakeup event パターンの導入。

| 変更 | ファイル | 内容 |
|------|----------|------|
| H1 | `engine_core.h` | `WAIT_PROCESS_EXIT = 4, WAIT_COUNT = 5` 追加 |
| H2 | `engine_core.h` | `ProcessPendingRemovals()` 宣言追加 |
| H3 | `engine_core.h` | `hWakeupEvent_`, `pendingRemovalPids_`, `pendingRemovalCs_` メンバ追加 |
| C1 | `engine_core.cpp` | コンストラクタに `hWakeupEvent_(nullptr)` 追加 |
| C2 | `engine_core.cpp` | `Initialize()` に auto-reset event 作成追加 |
| C3-C5 | `engine_core.cpp` | `EngineControlLoop()` 全面再構成 (Pattern B, final drain) |
| C6 | `engine_core.cpp` | `ProcessPendingRemovals()` 新規実装 (`for(;;)` ドレインループ) |
| C7 | `engine_core.cpp` | `OnProcessExit()` 全置換 (`pendingRemovalPids_` + `SetEvent`) |
| C8 | `engine_core.cpp` | `Stop()` に `hWakeupEvent_` CloseHandle 追加 |

**削除されたコード:**
- `deferredRemovalMutex_` / `deferredRemovalQueue_` (ヘッダ未宣言・ドレインなし)
- `case WAIT_TIMEOUT:` (INFINITE で不到達)
- `case WAIT_OBJECT_0 + WAIT_STOP: return;` (final drain スキップの dead code)
- `if (stopRequested_.load()) break;` (冗長チェック)
- `Sleep(1000)` (WAIT_FAILED の回避策)
- 旧 OnProcessExit コメントブロック (dead code)

**設計決定:**
- auto-reset event (`hWakeupEvent_`) — ResetEvent 不要
- Pattern B: WFMO 結果明示判定 (`WAIT_OBJECT_0 + WAIT_STOP` → break)。stopRequested_ は while 条件のセーフティネットとして残置
- `ProcessPendingRemovals()` は `for(;;)` 外側ループでキュー完全排出
- `OnProcessExit` コールバックは `CSLockGuard` + `SetEvent` のみ (軽量)
- `RemoveTrackedProcess` は EngineControlLoop スレッドからのみ呼出
- `Stop()` 内で `ProcessPendingRemovals()` は呼ばない (SCM ディスパッチャスレッド)
- race window: `UnregisterWaitEx(INVALID_HANDLE_VALUE)` が全 CB 完了を保証

## 8.1 Phase 2: UAF 防止 + 低負荷ハードニング — 完了 (2026/02/13)

**3 カテゴリ、23 変更。ビルド成功。コード構造監査 5/5 PASS。**

### カテゴリ 1: UAF 防止 (変更 1-16)

**問題**: `trackedProcesses_` が `unique_ptr` で管理されていたため、プロセス除去時にタイマーコールバックやウェイトコールバックが解放済みの `TrackedProcess` にアクセスする Use-After-Free の理論的リスクがあった。また `deferredTimerContext` のライフタイム管理が不完全（コールバック内 self-delete）だった。

**解決**:
- `trackedProcesses_` を `unique_ptr` → `shared_ptr` に変更
- `WaitCallbackContext` と `DeferredVerifyContext` に `shared_ptr<TrackedProcess>` を追加（コールバック中の早期破壊を防止）
- `deferredTimerContext` ポインタを `TrackedProcess` に追加し、決定的ライフタイム管理を実現
- `DeferredVerifyTimerCallback` から `delete context` を削除（`TrackedProcess::deferredTimerContext` で管理）
- deferred timer キャンセルを `nullptr`（非同期）→ `INVALID_HANDLE_VALUE`（同期）に変更
- `Stop()`, `CancelProcessTimers()`, `RemoveTrackedProcess()`, `DispatchEnforcementRequest` の全 cleanup 経路で `deferredTimerContext` を回収

| 変更 | ファイル | 内容 |
|------|----------|------|
| 1 | `engine_core.h` | `#include <memory>` 追加 |
| 2 | `engine_core.h` | `DeferredVerifyContext` を前方宣言に変更 |
| 3 | `engine_core.h` | `deferredTimerContext` フィールド追加 + `DeferredVerifyContext` を `TrackedProcess` 後に定義 |
| 4 | `engine_core.h` | `trackedProcesses_` を `shared_ptr` に変更 |
| 5 | `engine_core.cpp` | `WaitCallbackContext` に `shared_ptr<TrackedProcess>` 追加 |
| 6 | `engine_core.cpp` | `Stop()` で `deferredTimerContext` も回収 |
| 7 | `engine_core.cpp` | `DispatchEnforcementRequest` DEFERRED_VERIFICATION で fired context cleanup |
| 8 | `engine_core.cpp` | `ScheduleDeferredVerification` を shared_ptr ベースにリファクタ |
| 9 | `engine_core.cpp` | `CancelProcessTimers` deferred timer を `INVALID_HANDLE_VALUE` + delete に変更 |
| 10 | `engine_core.cpp` | `StartPersistentTimer` を shared_ptr ベースにリファクタ |
| 11 | `engine_core.cpp` | `DeferredVerifyTimerCallback` から `delete context` 削除 |
| 12 | `engine_core.cpp` | `ApplyOptimization` で `make_unique` → `make_shared` |
| 13-14 | `engine_core.cpp` | `ApplyOptimization`/`ReopenProcessHandle` で shared_ptr を `WaitCallbackContext` に渡す |
| 15-16 | `engine_core.cpp` | `RemoveTrackedProcess` で `deferredCtxToDelete` 追加 + `INVALID_HANDLE_VALUE` |

### カテゴリ 2: Wakeup Event 最適化 (変更 17-18)

- `OnProcessExit`: キュー empty→non-empty 遷移時のみ `SetEvent` (wasEmpty パターン)
- `EnqueueRequest`: 同上

### カテゴリ 3: 定常状態サイレンス (変更 19-23)

- `[SAFETY_NET] tick` デバッグログ削除
- ETW thread event デバッグログ削除
- `HandleSafetyNetCheck`: tracked process 0 件時に early return
- `RefreshJobObjectPids`: active Job Object 0 件時にスキップ
- Stats ログ: 全プロセス STABLE 時に抑制 (`aggressiveCount > 0 || persistentCount > 0`)

### コード構造監査結果

| # | 不変条件 | 結果 |
|---|---------|------|
| 1 | OnProcessExit は通知のみ (push + SetEvent) | **PASS** |
| 2 | TimerCallback は通知のみ (EnqueueRequest のみ) | **PASS** |
| 3 | UnregisterWaitEx(INVALID_HANDLE_VALUE) はコールバック外のみ | **PASS** |
| 4 | RemoveTrackedProcess はメインスレッドのみ | **PASS** |
| 5 | hWakeupEvent_ + WT_EXECUTEONLYONCE 維持 | **PASS** |

## 8.2 v1.01 バックログ消化 (4件) — 完了 (2026/02/13)

### 修正 1: Config 変更通知デバウンス (High — Bug Fix)

**問題**: `FindFirstChangeNotificationW(baseDir_)` はディレクトリ内の全ファイル変更で発火するため、
ログファイル書込みのたびに `HandleConfigChange()` → `HasFileChanged()` のファイルstat が実行されていた。
`HasFileChanged()` で INI ファイル以外は除外されるが、不要な WFMO ウェイクアップ自体が無駄。

**解決**: `configChangePending_` フラグ + `CONFIG_DEBOUNCE_MS` (2秒) デバウンス。

| 変更 | ファイル | 内容 |
|------|----------|------|
| H1 | `engine_core.h` | `lastConfigCheckTime_`, `configChangePending_` メンバ追加 |
| H2 | `engine_core.h` | `CONFIG_DEBOUNCE_MS = 2000` 定数追加 |
| C1 | `engine_core.cpp` | コンストラクタ初期化 |
| C2 | `engine_core.cpp` | `WAIT_CONFIG_CHANGE` case: 即時処理 → フラグセットに変更 |
| C3 | `engine_core.cpp` | switch 後にデバウンス経過判定 + `HandleConfigChange()` 呼出 |

**動作**: 通知受信 → フラグ ON → 次の WFMO ウェイクアップ時に 2 秒経過していれば処理。
最悪遅延 = SafetyNet 10秒 (実用上問題なし)。

### 修正 2: Stop() atomic 強化 (Medium — Phase 2 残)

**問題**: `running_.load()` ガードは理論上の並行 `Stop()` 呼出に対して形式的安全性がなかった。

**解決**: `compare_exchange_strong(expected, false)` に変更。

| 変更 | ファイル | 内容 |
|------|----------|------|
| C1 | `engine_core.cpp` | `running_.load()` → `running_.compare_exchange_strong(expected, false)` |
| C2 | `engine_core.cpp` | 関数末尾の `running_ = false;` 削除 (CAS で設定済み) |
| C3 | `engine_core.cpp` | TODO コメント (L256-261) 削除 |

**セマンティクス変更**: `running_` は `Stop()` 開始時に即座に `false` になる（旧: クリーンアップ完了後）。
`IsRunning()` はシャットダウン中に `false` を返すが、これは正しい動作。

### 修正 3: PERSISTENT 5秒タイマーに EcoQoS チェック追加 (Medium — Enhancement)

**問題**: `PERSISTENT_ENFORCE` は EcoQoS の状態に関わらず毎回 `PulseEnforceV6()` を実行していた。
EcoQoS OFF 時は冪等だが不要な SET 操作。

**解決**: `IsEcoQoSEnabled()` を先行チェックし、OFF なら enforce スキップ。

| 変更 | ファイル | 内容 |
|------|----------|------|
| C1 | `engine_core.cpp` | `PERSISTENT_ENFORCE` case を EcoQoS チェックファーストに再構成 |

**変更前**: 毎回 PulseEnforceV6 → 60秒後に IsEcoQoSEnabled (STABLE 復帰判定で 2回目の GET)
**変更後**: IsEcoQoSEnabled 1回 → ON なら enforce + `lastViolationTime` 更新、OFF ならスキップ → STABLE 復帰判定

**効果**: GET 1回に統一 (旧: 最大 2回)、EcoQoS OFF 時の無駄な SET 完全排除、`lastViolationTime` の正確な追跡。

### 修正 4: IPC パイプバッファサイズ整合 (Medium — Tech Debt)

**問題**: `CreateNamedPipe` のバッファサイズが 4096 だが、`UNLEAF_MAX_IPC_DATA_SIZE` は 65536。
`PIPE_TYPE_MESSAGE` では OS が自動拡張するため実害なしだが、意図が不明確。

**解決**: バッファサイズを `UNLEAF_MAX_IPC_DATA_SIZE` に統一。

| 変更 | ファイル | 内容 |
|------|----------|------|
| C1 | `ipc_server.cpp` | Input/Output buffer size を `UNLEAF_MAX_IPC_DATA_SIZE` に変更 |

## 8.3 v1.00 RC 改修 (回帰防止・KPI計測・可観測性) — 完了 (2026/02/14)

**目的**: 新機能追加なし。回帰防止・省電力KPI計測・体感悪化の最終潰し・最小可観測性を実装し、v1.00 リリース判定に必要なエビデンスを取得可能にする。

**変更ファイル (3ファイル):**

| ファイル | 変更内容 |
|----------|----------|
| `engine_core.h` | HealthInfo 拡張 (17フィールド追加)、カウンタメンバ 11個追加、エラー抑制マップ、`ERROR_LOG_SUPPRESS_MS` 定数、`#pragma detect_mismatch` ガード、`#include <utility>` |
| `engine_core.cpp` | Stop() 9ステップログ、タイマ/Wait 戻り値検証、起床カウンタ、PERSISTENT 適用/スキップカウンタ、エラーログ抑制、GetHealthInfo() 拡張、Stats ログ拡張、Config カウンタ |
| `ipc_server.cpp` | CMD_HEALTH_CHECK JSON 拡張 (phases/wakeups/enforcement/errors/config) |

### RC-P0: 回帰防止 (必須)

**Stop() シーケンスのステップログ:**
```
[STOP] Step 1: Stop signal sent
[STOP] Step 2: ETW monitor stopped
[STOP] Step 3: Control thread joined
[STOP] Step 4: Timer contexts collected (N deferred, M persistent)
[STOP] Step 5: Timer Queue deleted
[STOP] Step 6: Wait handles unregistered (K handles)
[STOP] Step 7: Job Objects cleaned up
[STOP] Step 8: Registry policies cleaned up
[STOP] Step 9: Event handles closed
[STOP] Complete: ShutdownWarnings=W
```

**タイマ/Wait 解除の戻り値検証:**
- `DeleteTimerQueueEx`, `DeleteTimerQueueTimer` (×4箇所), `UnregisterWaitEx` (×3箇所) の戻り値チェック
- 失敗時 `shutdownWarnings_` インクリメント (`ERROR_IO_PENDING` は正常として除外)

### RC-P1: 省電力 KPI 計測 (必須)

**イベント種別別起床カウンタ (4種):**
- `wakeupConfigChange_`, `wakeupSafetyNet_`, `wakeupEnforcementRequest_`, `wakeupProcessExit_`
- EngineControlLoop の各 WFMO case でインクリメント

**timeBeginPeriod 復活防止:**
- `#pragma detect_mismatch("UnLeaf_NoHighResTimer", "enforced")` をリンカレベルガードとして追加

### RC-P2: 体感悪化の最終潰し (推奨)

**PERSISTENT enforce 適用/スキップカウンタ:**
- `persistentEnforceApplied_`: EcoQoS ON → enforce 実行時
- `persistentEnforceSkipped_`: EcoQoS OFF → スキップ時

**エラーログ嵐防止:**
- `errorLogSuppression_` マップ: `(pid, errorCode) → lastLogTime`
- 同一 PID×エラーコードが 60秒 (`ERROR_LOG_SUPPRESS_MS`) 以内に再発した場合はログ抑制
- カウンタ (`error5Count_`, `error87Count_`) はログ抑制とは独立で常にインクリメント
- `RemoveTrackedProcess` でエントリ削除 (メモリリーク防止)

### RC-P3: 最小可観測性 (任意、効果大)

**HealthInfo 構造体拡張 (17+3フィールド):**
- フェーズ別: `aggressiveCount`, `stableCount`, `persistentCount`
- 起床: `wakeupConfigChange`, `wakeupSafetyNet`, `wakeupEnforcementRequest`, `wakeupProcessExit`
- Enforce: `persistentEnforceApplied`, `persistentEnforceSkipped`
- エラー: `shutdownWarnings`, `error5Count`, `error87Count`
- Config: `configChangeDetected`, `configReloadCount`
- §8.8 追加: `lastEnforceTimeMs` (Unix Epoch ms)、`activeProcessDetails` (構造化プロセス一覧)

**CMD_HEALTH_CHECK JSON 出力例 (§8.8 nlohmann/json 移行後):**
```json
{
  "schema_version": 1,
  "status": "healthy",
  "uptime_seconds": 3600,
  "engine": {
    "running": true,
    "mode": "NORMAL",
    "active_processes": [
      {"pid": 1234, "name": "chrome.exe", "phase": "STABLE", "violations": 2, "is_child": false},
      {"pid": 5678, "name": "chrome.exe", "phase": "STABLE", "violations": 0, "is_child": true},
      {"pid": 9012, "name": "chrome.exe", "phase": "PERSISTENT", "violations": 5, "is_child": true}
    ],
    "total_violations": 7,
    "phases": { "aggressive": 0, "stable": 2, "persistent": 1 }
  },
  "etw": { "healthy": true, "event_count": 1542 },
  "wakeups": { "config_change": 2, "safety_net": 360, "enforcement_request": 45, "process_exit": 8 },
  "enforcement": {
    "persistent_applied": 12, "persistent_skipped": 348,
    "total": 500, "success": 497, "fail": 3,
    "avg_latency_us": 42, "max_latency_us": 1200,
    "etw_thread_deduped": 156,
    "last_enforce_time_ms": 1740062400000
  },
  "errors": { "access_denied": 0, "invalid_parameter": 3, "shutdown_warnings": 0 },
  "config": { "changes_detected": 5, "reloads": 2 },
  "ipc": { "healthy": true }
}
```

**Stats ログ拡張:**
```
Stats: 3 tracked (A:0 S:2 P:1), 1 jobs, viol=7, wakeup(cfg:2 sn:360 enf:45 exit:8), persist(apply:12 skip:348)
```

**CMD_GET_STATS**: バイナリ形式維持 (Manager 後方互換)。拡張データは CMD_HEALTH_CHECK で取得。

## 8.4 ユニットテスト導入 (GoogleTest) — 完了 (2026/02/14)

**目的**: 回帰防止の自動化

**方式**: GoogleTest v1.15.2 (`FetchContent`)、`UNLEAF_BUILD_TESTS` CMake オプション

**変更ファイル (5ファイル):**

| ファイル | 変更内容 |
|----------|----------|
| `CMakeLists.txt` | GoogleTest FetchContent + テストターゲット (`gtest_add_tests`) |
| `src/common/config.h` | `friend class ::ConfigParserTest` 追加 (private メンバテスト用) |
| `tests/test_types.cpp` | 新規: 33 テスト (ProcessName/CriticalProcess/Phase/LogLevel/Config定数) |
| `tests/test_config.cpp` | 新規: 26 テスト (ParseIni 15 + BOM 3 + TargetList 8) |
| `tests/test_logger.cpp` | 新規: 8 テスト (FormatTimestamp/FormatLogEntry/LogLevel変換) |

**テスト結果**: 71/71 PASS

**設計判断**:
- `gtest_add_tests` 採用 (ソース静的解析、EXE 実行不要 — クロスコンパイル環境対応)
- `UNLEAF_BUILD_TESTS=OFF` デフォルト (本番ビルドへの影響ゼロ)

**エッジケース**: `.exe` は現行 `IsValidProcessName` で valid (documented)

**テスト実行コマンド**:
```
cmake -B build -DUNLEAF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 8.5 外部レビュー対応 — Enforcement テレメトリ (G1+G2) — 完了 (2026/02/20)

**背景**: ChatGPT + Gemini 外部レビューへの対応。5件のギャップのうち G1(レイテンシ計測) + G2(成功/失敗カウンタ) の2件を採用。G3-G5は不採用。

**変更ファイル**: `engine_core.h`, `engine_core.cpp`, `ipc_server.cpp` の3ファイル、6変更 (A-F)

| 変更 | 内容 |
|------|------|
| A | `HealthInfo` に5フィールド追加 |
| B | `EngineCore` にアトミックカウンタ5個追加 |
| C | `PulseEnforceV6` に QPC 計測 + 成功/失敗カウンタ |
| D | `GetHealthInfo` にテレメトリ転記 |
| E | Stats ログに enforce サマリ追加 |
| F | `CMD_HEALTH_CHECK` JSON enforcement セクション拡張 |

**検証結果**: `/W4` 警告ゼロ、71/71 テスト PASS

**不採用項目 (G3-G5):**
- G3: 設定変更イベントログ — 既存 `configReloadCount` で十分
- G4: プロセスライフサイクルログ — ログ肥大化リスク、デバッグ時に手動追加で対応可
- G5: ヘルスチェック定期自動実行 — サービス側自発ログは設計方針に反する (Manager からのオンデマンド取得で十分)

## 9. 過去バージョン履歴

### v7.93 (ログ可読性・ON/OFF制御)
- ライブログ水平スクロール対応
- ログ出力 ON/OFF トグル UI

### v7.92 (コンテキストメニュー・UI改善)
- ターゲットリスト・ログエリア右クリックメニュー
- .exe 自動付与、ダイアログ位置改善

### v7.91 (LogLevel バリデーション)
- 無効な LogLevel 値の警告出力

### v7.90 (品質改善・UI強化)
- ログフォーマット統一 [ERR ]/[ALRT]/[INFO]/[DEBG]
- INI バリデーション強化
- Manager 設定自動反映

### v7.80 (バグ修正・リファクタリング)
- メモリリーク修正、レースコンディション修正
- 入力バリデーション強化

### v7.70 (Health Check API)
- CMD_HEALTH_CHECK 追加

### v7.60 (構造化ログシステム)
- ログレベル導入

## 9.1 レジストリ一元管理 (RegistryPolicyManager) — 完了 (2026/02/08)

旧 `RegistryPolicyController` を `RegistryPolicyManager` に一元化。

| 変更 | ファイル | 内容 |
|------|----------|------|
| 新規 | `src/common/registry_manager.h` | RegistryPolicyManager クラス定義 |
| 新規 | `src/common/registry_manager.cpp` | 実装 (レジストリ操作 + マニフェスト永続化) |
| 削除 | `src/service/registry_policy.h` | 旧クラス → 移行済 |
| 削除 | `src/service/registry_policy.cpp` | 同上 |
| 編集 | `src/service/engine_core.h` | include パス変更 |
| 編集 | `src/service/engine_core.cpp` | 型名変更 + CleanupAll 修正 |
| 編集 | `src/manager/main_window.h` | include 追加 |
| 編集 | `src/manager/main_window.cpp` | OnStopService にセーフティネット追加 |
| 編集 | `CMakeLists.txt` | COMMON に追加、SERVICE から削除 |
| 編集 | `README.md` | レジストリ管理セクション追加 |

**設計ポイント:**
- `appliedPolicies_` は `map<wstring, wstring>` (exeName→fullPath) でインメモリ管理
- `UnLeaf_policies.ini` マニフェストでクラッシュ復旧対応
- SaveManifest はレジストリ書込の **前** に実行 (Scenario 14 crash-safety MUST)
- `RemoveAllPolicies` は完全冪等 (マニフェスト不存在/REGキー不存在/map 空すべて正常系)
- サービス停止時: インメモリから全削除 + マニフェスト削除
- サービス登録解除時: マニフェスト経由で全削除 (セーフティネット)

## 8.6 メモリリーク修正 — ゾンビ TrackedProcess 除去 — 完了 (2026/02/20)

**背景**: PDH カウンタによる 11.5 時間計測で Working Set が 7.43→9.86 MB (+2.43 MB) へ階段状増加。ハンドル数も 171→210 (+39)。

**根本原因**: `ApplyOptimization()` で `OpenProcess(SYNCHRONIZE)` が失敗した場合、`RegisterWaitForSingleObject` が未登録となり、プロセス終了時に `OnProcessExit` → `RemoveTrackedProcess` が呼ばれない。結果、`trackedProcesses_` にゾンビエントリが永久残留。

**修正内容**: `PerformPeriodicMaintenance()` に 60 秒間隔のプロセス生存確認を追加。

| 変更 | ファイル | 内容 |
|------|----------|------|
| H1 | `engine_core.h` | `lastProcessLivenessCheck_` メンバ追加 |
| H2 | `engine_core.h` | `LIVENESS_CHECK_INTERVAL = 60000` 定数追加 |
| C1 | `engine_core.cpp` | コンストラクタ初期化 `lastProcessLivenessCheck_(0)` |
| C2 | `engine_core.cpp` | `Start()` で `lastProcessLivenessCheck_ = now` 初期化 |
| C3 | `engine_core.cpp` | `PerformPeriodicMaintenance()` にプロセス生存確認ブロック追加 |

**動作フロー**:
1. `trackedProcesses_` を走査し `waitHandle == nullptr` のエントリを抽出
2. `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetExitCodeProcess` で終了判定
3. ハンドル取得不可またはプロセス終了を検出 → `pendingRemovalPids_` にキューイング
4. `SetEvent(hWakeupEvent_)` → 通常の `ProcessPendingRemovals` → `RemoveTrackedProcess` パスで安全に除去

**副次効果**:
- `errorLogSuppression_` のゾンビ PID エントリも `RemoveTrackedProcess` 内で自動クリーンアップ (Finding 2 解消)
- 既存の除去パイプラインを再利用するためロック競合なし

**検証結果**: ビルド成功、71/71 テスト PASS

## 8.7 CPUバースト解消 — エンフォースメントキュー最適化 — 完了 (2026/02/20)

**背景**: 対象プロセス（Chrome等）が短時間に数十〜数百スレッドを生成した際、ETW `OnThreadStart` が大量発火し、各イベントが `PulseEnforceV6` → `DisableThreadThrottling` → `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)` を呼び出すことで、CPU使用率が80%超にスパイクする問題。根本原因はSTABLEフェーズにレートリミットがなく、同一PIDへの重複リクエストもマージされないため、スナップショット生成が無制限に繰り返されること（実質O(N²)）。

**改善案の評価と決定**:
- **案1 (ETW threadId O(1)解除)**: 不採用。ETWヘッダの `ThreadId` は呼び出し元スレッドIDであり新規スレッドIDではない。UserDataパースはOSバージョン依存で脆く、EcoQoS再適用は全スレッドに影響するため単一スレッド修正では不十分。
- **案2 (キュー重複排除 + レートリミット)**: 採用（主要施策）。O(N²)→O(N)に削減。
- **案3 (IsEcoQoSEnabled マイクロキャッシュ)**: 採用（補助施策）。カーネル呼び出し80%削減。

**変更ファイル (3ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `engine_core.h` | `<vector>` インクルード、`ETW_STABLE_RATE_LIMIT=200`・`ECOQOS_CACHE_DURATION=100` 定数、`TrackedProcess` にキャッシュフィールド3つ (`ecoQosCached`, `ecoQosCachedValue`, `ecoQosCacheTime`)、`IsEcoQoSEnabledCached()` 宣言、`etwThreadDeduped_` カウンタ、`HealthInfo` に `etwThreadDeduped` フィールド |
| `engine_core.cpp` | `ProcessEnforcementQueue()` に同一PID `ETW_THREAD_START` 重複排除、STABLEフェーズに200msレートリミット、ETW_THREAD_STARTパスで `IsEcoQoSEnabledCached` 使用、`PulseEnforceV6` 後のキャッシュ無効化、`IsEcoQoSEnabledCached()` 実装、`GetHealthInfo()` にカウンタ出力 |
| `ipc_server.cpp` | `CMD_HEALTH_CHECK` JSON enforcement セクションに `etw_thread_deduped` 追加 |

**最適化効果**:
- 重複排除: バースト時にN個の同一PIDリクエスト → 1個にマージ
- レートリミット: STABLEフェーズ200ms間隔 (100イベント/秒→最大5回/秒)
- マイクロキャッシュ: `NtQueryInformationProcess` を100ms TTLでキャッシュ
- スレッド安全性: キャッシュ変数は全て `EngineControlLoop` スレッド内 + `trackedCs_` ロック下でのみアクセス

**検証結果**: ビルド成功、71/71 テスト PASS

## 8.8 UI/UX 改修 — nlohmann/json 導入・構造化JSON・RichEdit・リサイズ対応 — 完了 (2026/02/20)

**目的**: CMD_HEALTH_CHECK の JSON 出力を構造化し、Manager UI を拡張してリアルタイム可観測性を実現。

**変更ファイル (7ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `CMakeLists.txt` | nlohmann/json v3.11.3 FetchContent 追加、Service/Manager 両方にリンク |
| `engine_core.h` | `ActiveProcessDetail` 構造体追加、`HealthInfo` に `lastEnforceTimeMs`/`activeProcessDetails` 追加、`lastEnforceTimeMs_` アトミック追加、`<chrono>` インクルード |
| `engine_core.cpp` | `PulseEnforceV6` に system_clock ベース Unix Epoch ms タイムスタンプ記録、`GetHealthInfo` でプロセス詳細収集 |
| `ipc_server.cpp` | 手書き ostringstream JSON → nlohmann/json 完全移行、`schema_version:1`・`last_enforce_time_ms`・`active_processes` JSON Array of Objects 追加、例外フォールバック |
| `main_window.h` | `hRichEditLib_`・`hwndObservabilityStatus_`・`ID_OBSERVABILITY_STATUS` 追加、`RepositionControls()`/`UpdateObservabilityStatus()` 宣言、`WIN_MIN_WIDTH`/`WIN_MIN_HEIGHT` 追加 |
| `main_window.cpp` | RichEdit (RICHEDIT50W) 導入 (EDIT フォールバック付)、WS_OVERLAPPEDWINDOW でリサイズ対応、WM_SIZE/WM_GETMINMAXINFO ハンドラ追加、`RepositionControls()` 実装、Observability ステータスバー追加 (SN/Enf/Lat 表示)、最小化→トレイ連携 |

**追加要件の実現**:

| 要件 | 実装 |
|------|------|
| nlohmann/json 導入 | FetchContent v3.11.3、Service/Manager 両方リンク |
| Unix Epoch ミリ秒 | `std::chrono::system_clock` → `lastEnforceTimeMs_` (atomic) |
| active_processes 構造化 | `ActiveProcessDetail` → JSON Array of Objects (pid, name, phase, violations, is_child) |
| schema_version | `"schema_version": 1` トップレベル追加 |
| フォールバック処理 | `j.value()` デフォルト値、`try/catch` で JSON パースエラー処理 |

**検証結果**: ビルド成功 (警告ゼロ)、71/71 テスト PASS

## 8.9 ライブログ品質改修 — 差分追記・色分け・自動スクロール — 完了 (2026/02/20)

**背景**: §8.8 で EDIT → RICHEDIT50W に移行した際、RichEdit 固有の API 対応が不十分で 3 件の不具合が発生。
- ログ文字色が全て緑 (種別色分けなし)
- 自動スクロールが停止
- フォントが Engine/Manager 間で不一致

**設計方針**: 全文再描画 (`SetWindowTextW`) 禁止。差分追記方式で商用品質のログビューを実現。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `CriticalSection pendingLogCs_`・`std::vector<std::wstring> pendingLogLines_`・`size_t richEditLineCount_`・`bool autoScroll_` 追加。`MAX_RICHEDIT_LINES=5000` 定数。`GetLogLineColor()` 宣言。旧 `logLineBuffer_`/`logDisplayDirty_`/`MAX_LOG_LINES` 削除 |
| `main_window.cpp` | 下記全項目の実装 |

**実装詳細**:

| 項目 | 実装 |
|------|------|
| 差分追記 | `pendingLogLines_` (CriticalSection 保護) にキューイング → `RefreshLogDisplay()` で差分のみ `EM_REPLACESEL` 追加。`SetWindowTextW` 全文再描画禁止 |
| ログレベル色分け | `GetLogLineColor()`: `[ERR ]`→赤 `RGB(255,80,80)`, `[ALRT]`→橙 `RGB(255,170,0)`, `[INFO]`→UI標準色 `clrText_` (`RGB(220,220,225)`), `[DEBG]`→青 `RGB(120,170,255)`, タグなし(Manager)→水色 `RGB(180,210,255)` |
| フォント統一 | `CreateControls()` で `EM_SETCHARFORMAT(CFM_FACE\|CFM_SIZE)` により Consolas 11pt 設定。`WM_SETFONT` 不使用 |
| 自動スクロール | `autoScroll_` フラグ + `GetScrollInfo` で最下部判定。手動スクロール時は追従停止、最下部復帰で再開。`WM_VSCROLL(SB_BOTTOM)` で安定スクロール |
| 行数制限 | `MAX_RICHEDIT_LINES=5000`。超過時 `EM_LINEINDEX` で先頭から一括削除 (`trimEnd > 0 && trimEnd != -1` 安全チェック) |
| バッチ処理 | `WM_SETREDRAW FALSE/TRUE` で全行一括追加。タイマー(1s) にも `RefreshLogDisplay()` 統合 (キャッチオール) |
| キャレット最適化 | `GetWindowTextLengthW` はループ前1回のみ取得、`endPos += text.length()` でインクリメンタル追跡 (O(N)) |
| Undo 無効化 | `EM_SETUNDOLIMIT(0)` で Undo バッファ無効化。長時間稼働時のメモリ肥大防止 |

**スレッド安全性**: RichEdit 操作は全て UI スレッド (`WM_LOG_REFRESH`/`WM_TIMER` ハンドラ) からのみ実行。ワーカースレッド (`LogWatcherThread`) は `pendingLogLines_` への push + `PostMessageW` のみ。

**検証結果**: ビルド成功、71/71 テスト PASS

## 8.9.1 INFOログ色修正 — 完了 (2026/02/21)

**問題**: `GetLogLineColor()` で `[INFO]` タグに `clrLogText_`（緑 `RGB(0,255,128)`）を返していたため、INFOログが緑色で表示されていた。他のログレベルと比べて目立ちすぎ、通常ログとしての可読性が低下。

**修正**: `clrLogText_` → `clrText_` に変更。INFOログがUI標準色（薄グレー `RGB(220,220,225)`）で表示されるようになった。

| 変更 | ファイル | 内容 |
|------|----------|------|
| C1 | `main_window.cpp` L1324 | `return clrLogText_;` → `return clrText_;` |

**検証結果**: Releaseビルド成功、エラー・警告なし

## 8.10 Manager UI 最終安定化改修 (v3) — フォントメトリクス・DPI対応 — 完了 (2026/02/20)

**背景**: 固定ピクセル定数 (`BTN_HEIGHT=26`, `BTN_WIDTH=160`, `SMALL_BTN_WIDTH=60`) によるレイアウトが DPI 変更やフォント差異で崩れる問題を解消。

**設計方針**: コントロール高さ・ボタン幅をフォントメトリクス (`TEXTMETRICW`) から算出し、`CreateControls` と `RepositionControls` で同一計算式を使用。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `BTN_HEIGHT`/`BTN_WIDTH`/`SMALL_BTN_WIDTH` 削除。`fontHeight_`/`smallFontHeight_`/`buttonHeight_`/`labelHeight_`/`smallLabelHeight_`/`itemSpacing_`/`dpi_` メンバ追加。`SelectBestMonoFont()`/`MeasureTextWidth()`/`ComputeLayoutMetrics()`/`DpiScale()` 宣言追加 |
| `main_window.cpp` | 4関数新規実装、`CreateControls()`/`RepositionControls()` フォントメトリクスベースに全面改修、`WM_DPICHANGED` ハンドラ追加、`Initialize()` でモノフォント検出・メトリクス初期化 |

**実装詳細**:

| 項目 | 実装 |
|------|------|
| `ComputeLayoutMetrics()` | `GetDpiForWindow` + `TEXTMETRICW` から全レイアウト定数算出。NULLガード付き |
| `MeasureTextWidth()` | HDC+HFONT引数でフォント状態保証したテキスト幅計測 |
| `SelectBestMonoFont()` | Cascadia Mono → Cascadia Code → Consolas フォールバック検出 |
| `DpiScale()` | `MulDiv(value, dpi_, 96)` によるオーバーフロー安全DPIスケーリング |
| ボタン幅 | テキスト幅 + パディングから動的算出。最小幅保証 (`DpiScale(140)`/`DpiScale(48)`) |
| ListBox高さ | `fontHeight_ * 6 + DpiScale(6)` (テーマ余白補正)、`LB_SETITEMHEIGHT = fontHeight_ + DpiScale(2)` |
| `WM_DPICHANGED` | ウィンドウリサイズ → フォント再生成 → メトリクス再計算 → ListBox行高更新 → 再レイアウト |

**検証結果**: ビルド成功、71/71 テスト PASS

## 8.11 Manager UI 安定性強化 — GDI安全性・DPI安定性・再入防止 — 完了 (2026/02/20)

**背景**: §8.10 の DPI 対応実装に対し、GDI リソース管理の未定義動作排除、レイアウト再入防止、EnumChildWindows 適用範囲制限を追加。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | フォント StockObject フラグ追加 (§8.16 で `FontEntry.isStock` に統合済)、`layoutLocked_` フラグ追加 |
| `main_window.cpp` | `RecreateFontsForDpi()` StockFont安全削除 + `std::pair` 返却、デストラクタにStockFontガード、`WM_DPICHANGED` に `layoutLocked_` 制御追加、`RepositionControls()` に再入ガード、`EnumChildWindows` を直下子ウィンドウ限定 (2箇所)、`#include <utility>` 追加 |

**実装詳細**:

| 項目 | 実装 |
|------|------|
| StockFont安全削除 | `safeCreateFont` ラムダが `std::pair<HFONT, bool>` を返却。`CreateFontW` 失敗時に `DEFAULT_GUI_FONT` フォールバック + `isStock=true`。`DeleteObject` 前に `!isStock` チェック |
| レイアウト再入防止 | `layoutLocked_` を `WM_DPICHANGED` スコープで true/false 制御。`RepositionControls()` 冒頭で `if (layoutLocked_) return;`。WM_SETFONT 連鎖による二重発火防止 |
| EnumChildWindows制限 | `GetParent(hwnd) != self->hwnd_` で直下子のみ対象。RichEdit/ListBox 内部コントロールへの誤適用防止 |
| GetDC NULLガード | `ComputeLayoutMetrics()`/`CreateControls()`/`RepositionControls()` の3箇所。NULL時は安全な既定値で続行 |
| DpiScale | `MulDiv(value, dpi_, 96)` — Win32標準のオーバーフロー安全関数 |

**検証結果**: ビルド成功、71/71 テスト PASS

## 8.12 DPI変更時の描画停止 — RedrawGuardスコープガード — 完了 (2026/02/20)

**背景**: DPI変更時にフォント再生成・レイアウト更新中に描画チラつきが発生。`WM_SETREDRAW`で描画を一時停止し、処理完了後に`RedrawWindow`で一括再描画することで解消。

**変更ファイル (1ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.cpp` | `WM_DPICHANGED`ハンドラにRedrawGuardスコープガード導入、親子コントロールWM_SETREDRAW制御、InvalidateRect→RedrawWindow置換 |

**実装詳細**:

| 項目 | 実装 |
|------|------|
| RedrawGuardスコープガード | ローカル構造体 (`parent`, `child1`, `child2`)。デストラクタ (`noexcept`) で child1→child2→parent の順に `WM_SETREDRAW TRUE` 復帰 + `RedrawWindow(RDW_INVALIDATE \| RDW_ALLCHILDREN \| RDW_UPDATENOW)`。null/破棄済みウィンドウチェック付き。early-return/例外でも安全 |
| 親子コントロール一括RAII | `RedrawGuard guard{ hwnd_, hwndLogEdit_, hwndTargetList_ }` で親子3ウィンドウを一括管理。手動復帰コード不要 |
| RDW_ERASE除外 | 将来のダークテーマ/カスタム背景拡張時のフラッシュ回避 |
| layoutLocked_配置 | guard発動前に`false`設定 — 固着防止かつ再入防止 |

**検証結果**: ビルド成功、71/71 テスト PASS

## 8.13 ライブログフォント改修 — 定数集約・9pt化・フォント優先順位修正 — 完了 (2026/02/21)

**背景**: ライブログのフォントサイズ（8pt）とフォント名がコード内の複数箇所にハードコードされており、変更時に同期漏れのリスクがあった。また8ptは現代基準で小さく、フォント優先順位もリガチャ付きの Cascadia Code が最優先になっていた。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `LOG_FONT_PT=9`・`LOG_FONT_TWIPS=LOG_FONT_PT*20` 定数追加 |
| `main_window.cpp` | 4箇所のハードコード値 (`-11`, `8*20`) を定数に置換、`SelectBestMonoFont()` フォント優先順位変更 |

**実装詳細**:

| 項目 | 実装 |
|------|------|
| フォントサイズ定数集約 | `LOG_FONT_PT=9` (pt)、`LOG_FONT_TWIPS=LOG_FONT_PT*20` (RichEdit twips)。`CreateFontW` は `-LOG_FONT_PT`、RichEdit CHARFORMAT は `LOG_FONT_TWIPS` で統一 |
| 9pt化 | `CreateFontW(-11,...)` → `-LOG_FONT_PT` (2箇所)、`cf.yHeight = 8*20` → `LOG_FONT_TWIPS` (2箇所) |
| フォント優先順位修正 | `Cascadia Code → Cascadia Mono` → `Cascadia Mono → Cascadia Code`。リガチャ (`!=`→`≠`, `<=`→`≤`) がログ誤読を招くため Mono を優先 |

## 8.14 UIコンパクト化 — 幅520px・表示精度・オーバーフロー検知 — 完了 (2026/02/21)

**背景**: §8.8 で `WIN_MIN_WIDTH` を 800 に設定したが、方針転換しコンパクトUIを目指す。初回500px設定後、高DPI安全余白・レイテンシ表示精度・運用ログ可読性の品質修正を実施。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `WIN_WIDTH`/`WIN_MIN_WIDTH` 540 (§8.15で520→540に拡大)、`WIN_MIN_HEIGHT` 480 (初期サイズと統一) |
| `main_window.cpp` | ステータス文字列是正 (SN/Enforcement/Latency)、レイテンシ3段階表示、レイアウトオーバーフロー検知 |

**実装詳細**:

| 項目 | 変更前 | 変更後 |
|------|--------|--------|
| ウィンドウ幅 | `WIN_WIDTH = 500` | `WIN_WIDTH = 540` (§8.15で520→540に拡大) |
| 最小幅 | `WIN_MIN_WIDTH = 500` | `WIN_MIN_WIDTH = 540` |
| 最小高さ | `WIN_MIN_HEIGHT = 400` | `WIN_MIN_HEIGHT = 480` (初期サイズと統一) |
| ステータス文字列 | `Safety: X \| Blocks: Y (Z err) \| Lat: Wms` | `SN: X \| Enforcement: Y (Z fail) \| Latency: -/<1ms/Wms` |
| レイテンシ表示 | `avgUs/1000` ms (0ms問題あり) | 未計測: `-`、<1ms: `<1ms`、それ以上: `Wms` |
| オーバーフロー検知 | なし | `MeasureButtons()` 後にボタン幅 vs contentWidth 検知。`static bool` で1回限り通知、OutputDebugStringW + AppendLog 二重出力 |

**レイアウト安全性**: contentWidth = 540 - 16*2 = 508px。サービスボタン2個・ターゲットボタン3個ともに収容可能。

**検証結果**: ビルド成功

## 8.17 フォントDPIスケーリング修正 + トグルスイッチGDI+化 — 完了 (2026/02/22)

**背景**: 初回起動時にフォントがDPIスケーリングされず96dpi固定サイズで描画される問題。またトグルスイッチがGDIベースでアンチエイリアスなし・OSテーマ色依存だった。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `#include <objidl.h>` + `#include <gdiplus.h>` + `#pragma comment(lib, "gdiplus.lib")` 追加 |
| `main_window.cpp` | Initialize() フォント初期化簡素化、RecreateFontsForDpi() nullガード、WM_SETTINGCHANGE SPI フィルタ、DrawToggleSwitch() GDI+化 |

**修正A: フォントDPIスケーリング**:

| 項目 | 変更前 | 変更後 |
|------|--------|--------|
| Initialize() | プレウィンドウで5フォント手動生成 + ウィンドウ後にmono手動生成 | プレウィンドウは `DEFAULT_GUI_FONT` のみ → ウィンドウ後に `RecreateFontsForDpi()` で全フォント一括生成 |
| RecreateFontsForDpi() | コントロール固有フォント適用にnullガードなし | `hwndStatusLabel_` 等 5箇所に null ガード追加 (初回呼び出し時の安全性確保) |
| WM_SETTINGCHANGE | highContrast_ + toggle再描画のみ | `SPI_SETNONCLIENTMETRICS` / `SPI_SETICONTITLELOGFONT` の場合にフォント再生成+レイアウト再計算を追加 |

**修正B: トグルスイッチGDI+化**:

| 項目 | 変更前 | 変更後 |
|------|--------|--------|
| 初期化 | なし | `static` ローカル変数による `GdiplusStartup` once 初期化 |
| 描画方式 | GDI (`RoundRect`/`Ellipse`) | GDI+ (`GraphicsPath`/`FillPath`/`FillEllipse`) + `SmoothingModeAntiAlias` + `PixelOffsetModeHalf` |
| 背景 | GDI `FillRect` | GDI+ `Graphics::Clear()` |
| トグルサイズ | 固定 `DpiScale(28)×DpiScale(14)` | `smallFontHeight_ × 0.85` × `trackH × 1.8` (フォントメトリクス連動) |
| 配色 | `GetSysColor(COLOR_HIGHLIGHT/COLOR_3DSHADOW/COLOR_WINDOW)` | Cyan Resonance: ON `RGB(80,220,220)` OFF `RGB(61,61,70)` Thumb `RGB(240,240,245)` + Hover対応 (§8.19) |
| テキスト-トグル間隔 | `DpiScale(34)` 固定 | `trackX - DpiScale(kGapSmall)` (ボタン間隔と統一) |

## 8.19 Cyan Resonance — トグルスイッチ カラーリング変更 + Hover対応 — 完了 (2026/02/22)

**背景**: Log Output トグルスイッチの ON/OFF カラーを、ライブログの `[ UI ]` シアン色と視覚的にリンクさせる「Cyan Resonance」テーマに変更。併せてホバーエフェクトを新規導入。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `toggleHovered_` メンバ追加 (ホバー状態追跡用) |
| `main_window.cpp` | `ToggleSubclassProc` に `WM_MOUSEMOVE`/`WM_MOUSELEAVE` ハンドラ追加、`DrawToggleSwitch` トラック色を Cyan Resonance テーマに変更 |

**カラー変更**:

| 状態 | 変更前 | 変更後 (通常) | 変更後 (Hover) |
|------|--------|--------------|---------------|
| ON | `RGB(46,204,113)` Spring Green | `RGB(80,220,220)` Cyan #50DCDC | `RGB(100,235,235)` #64EBEB |
| OFF | `RGB(70,70,80)` Neutral dark | `RGB(61,61,70)` Dark steel #3D3D46 | `RGB(75,75,85)` #4B4B55 |
| Thumb | `RGB(240,240,245)` | 変更なし | 変更なし |

**ホバーエフェクト実装**:
- `WM_MOUSEMOVE` → `toggleHovered_ = true` + `TrackMouseEvent(TME_LEAVE)` + `InvalidateRect`
- `WM_MOUSELEAVE` → `toggleHovered_ = false` + `InvalidateRect`
- Hover色は通常色の約+20明度で統一感を維持

**検証結果**: 未ビルド (ユーザー確認待ち)


## 8.20 削除ボタンY座標修正 + 初期フォーカス設定 — 完了 (2026/02/22)

**背景**: §8.17 で tgtBtnAreaH を変更したが方針転換。tgtBtnAreaH は元の `2 * inset` に戻し、削除ボタンのY座標を直接 -1px する方式に変更。併せて Tab キーによるボタン間フォーカス移動のため、起動時の初期フォーカスを設定。

**変更ファイル (1ファイル、5箇所)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.cpp` | tgtBtnAreaH 復元 (2箇所)、削除ボタンY座標 -1px (2箇所)、初期フォーカス設定 (1箇所) |

**修正詳細**:

| # | 箇所 | 変更内容 |
|---|------|----------|
| 1 | `CreateControls` L860 | `tgtBtnAreaH = tgtBtnH * 3 + tgtBtnGap * 2 + inset` → `+ 2 * inset` に復元 |
| 2 | `RepositionControls` L1040 | 同上 |
| 3 | `CreateControls` L883 | `btnStartY + (tgtBtnH + tgtBtnGap) * 2` → `- 1` 追加 (削除ボタンのみ) |
| 4 | `RepositionControls` L1051 | 同上 |
| 5 | `Initialize` L272 | `UpdateWindow(hwnd_)` の後に `SetFocus(hwndBtnAdd_)` 追加 |

**設計判断**:
- WM_GETDLGCODE サブクラスは不要 (BUTTON + BS_OWNERDRAW + WS_TABSTOP は標準でTab対応)
- SetFocus は ShowWindow/UpdateWindow の後に配置 (ウィンドウ表示後でないとフォーカス設定が無効)

**検証結果**: Releaseビルド成功

## 8.21 レイアウト整列ルール強化 + ボタンサイズ統一 — 完了 (2026/02/23)

**背景**: ManagerUI の5ボタン (Start/Stop/Add/Select/Remove) が幅・高さとも2系統に分かれていた。また対象リスト右端とStartボタン右端、ターゲットボタン左端とStopボタン左端が独立計算で整列が保証されていなかった。

**変更ファイル (1ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.cpp` | `MeasureButtons()` コメント追加、`CreateControls()` / `RepositionControls()` レイアウト計算式変更 |

### Phase 1: レイアウト整列ルール強化

**整列ルール**:
- リスト右端 = Startボタン右端 (`listW = btnX - gap - MARGIN`)
- ターゲットボタン左端 = Stopボタン左端 (`btnX = stopX`)
- 安全クランプ: `btnX = max(btnX, MARGIN)`
- 最小リスト幅: `listW = max(listW, DpiScale(120))`

**座標計算チェーン** (右端→逆算):
```
stopX  = MARGIN + contentWidth - unifiedBtnW
startX = stopX - gap - unifiedBtnW
btnX   = stopX
listW  = btnX - gap - MARGIN
```

### Phase 2: ボタンサイズ統一

**幅の統一**:
```cpp
int unifiedBtnW = (std::max)({DpiScale(140), bw.startW, bw.stopW, bw.addW, bw.selectW, bw.removeW});
```
- 5ボタン全測定値の最大値を採用、最低幅 `DpiScale(140)` を保証

**高さの統一**:
```cpp
int listHeightBase = fontHeight_ * 6 + DpiScale(6);
int tgtBtnGap = DpiScale(kGapLarge);
int inset = DpiScale(1);
int unifiedBtnH = static_cast<int>((listHeightBase - 2 * inset - tgtBtnGap * 2) / 3.0f + 0.5f);
```
- 既存 tgtBtnH 式をそのまま使用、全5ボタンに適用
- 新メンバ変数なし、main_window.h 変更なし

**維持された仕様**:
- StatusLabel高さ: `buttonHeight_` のまま（変更なし）
- `listHeight = max(listHeight, tgtBtnAreaH)` ガード: 維持
- overflow detection: `unifiedBtnW * 2 + gap` ベースに更新

**検証結果**: Releaseビルド成功、警告ゼロ

## 8.18 ダークモード固定化 — OSテーマ依存排除 — 完了 (2026/02/22)

**背景**: ダーク固定UIアプリとしての設計意図を確定し、OSテーマ依存による描画不整合を完全排除。

**変更ファイル (3ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.cpp` | カラーパレット値更新、`wc.hbrBackground = nullptr`、`WM_ERASEBKGND` → `return TRUE` のみ、`OnPaint()` 背景描画一本化 |
| `docs/UI_RULES.md` | 新規作成: ダーク固定ポリシー + 描画責任ルール |

**カラーパレット更新**:

| 変数 | 変更前 | 変更後 |
|------|--------|--------|
| `clrBackground_` | `RGB(25,25,30)` | `RGB(32,32,36)` |
| `clrPanel_` | `RGB(35,35,42)` | `RGB(45,45,52)` |
| `clrText_` | `RGB(220,220,225)` | `RGB(230,230,235)` |
| `clrTextDim_` | `RGB(140,140,150)` | `RGB(150,150,160)` |
| `clrAccent_` | 変更なし | 状態色 `clrOnline_` とは分離維持 |

**描画責任統一**:

| 項目 | 変更前 | 変更後 |
|------|--------|--------|
| `wc.hbrBackground` | `hBrushBg_` (OS管理) | `nullptr` (OS自動塗り停止) |
| `WM_ERASEBKGND` | 未実装 → FillRect追加 → 削除 | `return TRUE` のみ (OS erase 無効化専用、描画禁止) |
| `OnPaint()` | FillRect → 削除 → 復元 | `BeginPaint` 直後に `FillRect(hdc, &rc, hBrushBg_)` (唯一の背景描画箇所) |

**設計ルール (docs/UI_RULES.md)**:
- UnLeaf はダーク固定アプリ。OSテーマ非追従
- `GetSysColor()` / `COLOR_*` 使用禁止
- 全描画は `WM_PAINT` 内のみ。`WM_ERASEBKGND` は描画禁止
- 背景は `WM_PAINT` 冒頭で塗る (ダブルバッファリング対応前提)

**検証結果**: ビルド成功


## 8.16 Manager UI 最終UX改善 — ロールベース色管理・フォント構造化・情報階層整理 — 完了 (2026/02/22)

**背景**: 状態認識の即時性・視線導線最適化・情報階層の明確化を目的としたUX改善。既存ロジック・DPI耐性・Win32ネイティブ互換性を維持。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.h` | `ControlRole` enum 追加、`FontEntry`/`FontSet` 構造体導入（旧個別フォントメンバ置換）、`GetControlRole()`/`ResolveRoleColor()` 宣言追加 |
| `main_window.cpp` | 修正1〜3 実装、フォント管理全面移行、WM_CTLCOLORSTATIC ロールベース化 |

**改修内容 (4項目)**:

| # | 改修 | 内容 |
|---|------|------|
| 1 | Log Output ラベル右寄せ | `DrawToggleSwitch()` の `DT_LEFT` → `DT_RIGHT`。トグルスイッチ直前に密着配置 |
| 2 | ステータス文字列簡潔化 | `"Active: N processes"` → `"Processes: N"` (3箇所 + 初期テキスト) |
| 3 | サービス状態太字+色分け | `fonts_.bold` (FW_BOLD, 12pt) を `hwndStatusLabel_` に適用。Stopped→グレー、Unknown→赤に変更 |
| 4 | レイアウト順序確認 | 既に指示通り (状態→ボタン→プロセス→ログ→統計) のため変更なし |

**設計改善 (ユーザー指示による構造改善)**:

| 項目 | 実装 |
|------|------|
| ControlRole enum | `ServiceState`/`Diagnostic`/`Observability`/`Toggle`/`Default` の5ロール。`GetControlRole()` でHWND→ロール解決、`ResolveRoleColor()` でロール×状態から色決定。WM_CTLCOLORSTATIC は3行に集約 |
| FontSet 構造体 | `FontEntry` (handle + isStock + Reset()) を持つ `FontSet` に title/normal/bold/sm/mono を統合。`RecreateFontsForDpi()` で全フォントを一括再生成。デストラクタは `fonts_.ResetAll()` のみ。`sm` 命名は Win32 SDK の `small` マクロ (`rpcndr.h`) 回避 |
| 色ロジック分離 | `hwndStatusLabel_` (ControlRole::ServiceState): Stopped→グレー (低警報)、Unknown→赤 (エラー)。`hwndEngineStatus_` (ControlRole::Diagnostic): 現行色維持 (Stopped=赤、異常検知性保持) |

**削除されたコード**:
- `hFontTitle_`/`hFontNormal_`/`hFontSmall_`/`hFontMono_` 個別メンバ → `fonts_.title`/`.normal`/`.sm`/`.mono` に移行
- `fontTitleIsStock_`/`fontNormalIsStock_`/`fontSmallIsStock_`/`fontMonoIsStock_` → `FontEntry.isStock` に統合
- WM_CTLCOLORSTATIC の個別 if 分岐 (4ブロック) → `ResolveRoleColor(GetControlRole(hwndCtrl))` 1行

**検証結果**: Releaseビルド成功、71/71 テスト PASS

## 8.15 Manager UI 総合改修 — Toggle背景・英語化・行間・スクロールバー・トグルスイッチ・幅拡大 — 完了 (2026/02/21)

**背景**: ダークテーマ環境で Log Output トグルの背景が白く描画される問題を起点に、UI全体の品質改修を一括実施。

**改修内容 (6項目)**:

| # | 改修 | 状態 | 内容 |
|---|------|------|------|
| 1 | Toggle描画テキスト背景修正 | 本セッション修正 | `DrawToggleSwitch()` の `FillRect` を `GetSysColorBrush(COLOR_BTNFACE)` → `CreateSolidBrush(clrBackground_)` に変更。テキスト色を `GetSysColor(COLOR_BTNTEXT)` → `clrText_` に変更。GDIリーク防止の `DeleteObject` 追加 |
| 2 | UIメッセージ英語化 | 先行セッションで完了済 | `AppendLog()` 経由のライブログメッセージを全て英語化。MessageBox等は日英バイリンガル維持 |
| 3 | 行間統一 | 先行セッションで完了済 | `SPF_SETDEFAULT` で既定段落フォーマット登録、`RefreshLogDisplay()` ループ内で `EM_SETPARAFORMAT` 適用、再入防止ガード追加 |
| 4 | スクロールバー連動表示 | 先行セッションで完了済 | `UpdateScrollBarVisibility()` + `PostMessage(WM_APP_UPDATE_SCROLLBAR)` 遅延評価。`GetScrollInfo` の `nMax/nPage` 判定 |
| 5 | Log Output トグルスイッチ化 | 先行セッションで完了済 | `SetWindowSubclass` + ダブルバッファ描画。高コントラスト時はネイティブフォールバック。`WM_SETTINGCHANGE` で動的切替 |
| 6 | ウィンドウ幅拡大 | 先行セッションで完了済 | `WIN_WIDTH`/`WIN_MIN_WIDTH` を 520→540 に変更 |

**変更ファイル (1ファイル — 本セッション分)**:

| ファイル | 変更内容 |
|----------|----------|
| `main_window.cpp` L1550-1557 | `DrawToggleSwitch()` 背景ブラシを `clrBackground_` に、テキスト色を `clrText_` に変更 |

**検証結果**: コンパイル成功 (リンクは exe 実行中のためスキップ)

## 8.22 Service Working Set 増加修正 — コンテナ無制限蓄積の解消 — 完了 (2026/02/27)

**背景**: PDH カウンタで 26 時間計測 (0225.csv: 2026-02-24 08:36 〜 2026-02-25 11:06) した結果、
`Working Set - Private` が 5.22 MB → 10.08 MB (+4.86 MB) と単調増加していた。
増加はプロセス活動量に比例し（夜間 01:00〜05:21 は 9.16 MB で完全静止）、
フロアレベルが約 0.19 MB/時間で上昇し続けることを確認。

**根本原因**: メモリリーク（`new` 忘れ）ではなく、2 つのコンテナが設計上の上限なく蓄積し続けていた。

| コンテナ | 型 | 問題 |
|---------|-----|------|
| `errorLogSuppression_` | `std::map<pair<DWORD,DWORD>, ULONGLONG>` | `(pid, errorCode)` ペアを永続追加。プロセス終了時のみ削除。TTL 期限切れエントリが残存 |
| `policyAppliedSet_` | `std::set<std::wstring>` | 実行ファイル名を永続追加。`Stop()` 時のみクリア |

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `src/service/engine_core.h` | `SUPPRESSION_CLEANUP_INTERVAL`・`SUPPRESSION_TTL`・`SUPPRESSION_MAX_SIZE`・`POLICY_SET_MAX_SIZE` 定数追加、`lastSuppressionCleanup_` メンバ追加 |
| `src/service/engine_core.cpp` | コンストラクタ初期化、`PerformPeriodicMaintenance()` に TTL クリーンアップ追加、`HandleConfigChange()` に swap クリア追加、`ApplyRegistryPolicy()` にサイズ上限追加 |

**実装詳細**:

| 変更 | 内容 |
|------|------|
| `errorLogSuppression_` TTL クリーンアップ | 60 秒毎に 5 分 (`SUPPRESSION_TTL = 300000`) 超過エントリを `erase`。TTL を抑制時間 (60s) より長く設定し再登録ループ防止 |
| `errorLogSuppression_` 保険上限 | 2000 エントリ超過時は **最古から順に削除** (`erase(begin())`)。`clear()` はログ嵐を招くため不採用 |
| `policyAppliedSet_` config reload クリア | `HandleConfigChange()` で `std::set<std::wstring>().swap(policyAppliedSet_)` → clear + capacity 解放。Working Set の戻りを改善 |
| `policyAppliedSet_` サイズ上限 | 1000 エントリ超過時に同様の swap クリア。`ApplyRegistryPolicy` は冪等なので再適用は無害 |

**新定数**:
```cpp
static constexpr ULONGLONG SUPPRESSION_CLEANUP_INTERVAL = 60000;  // 60s cleanup cadence
static constexpr ULONGLONG SUPPRESSION_TTL = 300000;               // 5min TTL
static constexpr size_t    SUPPRESSION_MAX_SIZE = 2000;            // emergency cap
static constexpr size_t    POLICY_SET_MAX_SIZE = 1000;
```

**期待効果**:

| コンテナ | 修正前 | 修正後 |
|---------|--------|--------|
| `errorLogSuppression_` | 無制限蓄積 | 最大 ~144 KB で安定 |
| `policyAppliedSet_` | 無制限蓄積 | 最大 ~260 KB で安定 |
| 26 時間での増加量 | +4.9 MB | +0.4 MB 以下（ヒープ自然変動のみ）|

**検証方法**: ビルド後に同条件で PDH 再計測。`[DIAG] errSup=` の値が時間経過で増加しないことを確認。

---

## 8.23 IPC 最適化 & メモリ診断ログ — 完了 (2026/03/01)

**背景**: ChatGPT IPC 最適化提案 + ユーザー追加要件。
- `UpdateEngineStatus` が WM_TIMER 経由で毎秒 2 回呼ばれ、パイプ open/close が 7200 回/時発生
- IPC 品質（レイテンシ・再接続率）が経時的に変化するか観測できない
- Service 側のメモリ使用量（§8.22 修正効果）をログで追跡できない

**採否判定**:

| 提案 | 判定 |
|------|------|
| A-1: UpdateEngineStatus 二重呼び出し削除 | 採用 |
| A-2: 永続 Pipe | 却下（サーバーがシングルショット設計） |
| B-1: HeapWalk | 却下（全スレッド停止） |
| B-2: wait カウンタ | 実装済み（§8.22） |
| B-3: IPC カウンタ | 採用 |

**変更ファイル (4ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `src/manager/main_window.h` | `WM_IPC_REFRESH = WM_APP+1` 定数追加、IPC レート制御メンバ 7 本追加 (`lastIpcRefreshTime_`/`lastIpcStatLog_`/`ipcRefreshPending_`/`prev*` 4 本) |
| `src/manager/main_window.cpp` | A-1: `UpdateServiceStatus()` から `UpdateEngineStatus()` 呼び出し削除。WM_TIMER: 3 秒ゲートで `PostMessage(WM_IPC_REFRESH)`。WM_IPC_REFRESH ハンドラ新設（IPC 実行 + 60 秒毎 `[IPC]`/`[IPCQ]` ログ） |
| `src/manager/ipc_client.h` | atomic カウンタ 5 本 (`openCount_`/`totalRequests_`/`successfulRequests_`/`failedRequests_`/`reconnectOpenFail_`)、QPC レイテンシ計測メンバ、再接続理由カウンタ 3 本、`QualityStats` struct、getter、`SnapAndResetQuality()` 宣言 |
| `src/manager/ipc_client.cpp` | コンストラクタに `QueryPerformanceFrequency`。`SendCommand()` 全面書き換え（lock 縮小: pipeHandle_ swap のみロック内、I/O はロック外、goto cleanup 統一）。`SnapAndResetQuality()` 実装（atomic exchange でリセット付きスナップショット）。`config.h` include 追加 |
| `src/service/engine_core.h` | `lastMemLogTime_` メンバ追加（`lastSuppressionCleanup_` 直後）。`MEM_LOG_INTERVAL_SHORT=10000`/`MEM_LOG_INTERVAL_LONG=60000`/`MEM_LOG_WARMUP_MS=1800000` 定数追加 |
| `src/service/engine_core.cpp` | `psapi.h` include + `psapi.lib` pragma 追加。コンストラクタ/`Start()` に `lastMemLogTime_` 初期化。`PerformPeriodicMaintenance()` DIAG ブロック直後に [MEM] ログブロック追加（起動 30 分: 10 秒間隔、以降: 60 秒間隔） |

**実装詳細**:

### A-1: UpdateEngineStatus 重複削除

`UpdateServiceStatus()` 内の `UpdateEngineStatus()` 呼び出しを削除。
WM_IPC_REFRESH ハンドラから呼ばれるため不要。

### WM_IPC_REFRESH レートリミット

```
WM_TIMER (1秒) → UpdateServiceStatus() のみ
                → 3秒経過 & pending なし → PostMessage(WM_IPC_REFRESH)

WM_IPC_REFRESH → UpdateEngineStatus() + UpdateObservabilityStatus()
              → 60秒毎に [IPC]/[IPCQ] ログ出力
```

### SendCommand() ロック縮小設計

```cpp
// ① pipeHandle_ を localPipe に swap（ロック内のみ）
// ② WriteFile / ReadFile はロック外で実行
// cleanup: pipeHandle_ == localPipe の場合のみ INVALID に戻す
//          → 常時 CloseHandle(localPipe) で単一責任
```

### [MEM] ログ出力例（LOG_DEBUG 時）

```
[MEM] pid=1234 priv=5242880 rss=8192000 commit=5242880 handles=156 policy=12 errSup=0
[IPC] req=20 open=20 succ=20 fail=0 rate=0.33/s
[IPCQ] latency=0.5ms reconnect(timeout=0 broken=0 manual=0 openFail=0)
```

**成功判定（48 時間稼働）**:

| 条件 | ログ |
|------|------|
| avg IPC レスポンスが単調増加しない | `[IPCQ] latency=` 推移 |
| 再接続率が単調増加しない | `[IPCQ] reconnect(...)` 推移 |
| commit サイズが線形増加しない | `[MEM] commit=` 推移 |
| waitDelta が単調増加しない | `[DIAG] wait(delta:...)` |

---

## 8.24 P0 最終安定化 — win_string_utils・例外ログ・IPC統計 #ifdef 化 — 完了 (2026/03/02)

**目的**: v1.00 リリース前安定化。機能追加なし・仕様変更なし。影響範囲は指定箇所のみ。

**変更ファイル (6ファイル)**:

| ファイル | 変更内容 |
|----------|----------|
| `src/common/win_string_utils.h` | 新規作成: `unleaf::Utf8ToWide(const char*)` 宣言 |
| `src/common/win_string_utils.cpp` | 新規作成: UTF-8 → UTF-16 変換実装。バグ修正済み（後述） |
| `CMakeLists.txt` | `COMMON_SOURCES` に `win_string_utils.cpp` 追加、コメントアウト済み `#  src/manager/UnLeaf.rc` 削除 |
| `src/manager/main_window.h` | 5メンバ (`lastIpcStatLog_`/`prevIpc*` 4本) を `#ifdef INTERNAL_DIAG` で囲う |
| `src/manager/main_window.cpp` | `#include "../common/win_string_utils.h"` 追加、`UpdateObservabilityStatus()` catch を `std::exception&` + `...` の2段に改善・`LOG_ERROR` 追加、`WM_IPC_REFRESH` ハンドラから AppendLog/delta計算/buf/swprintf を削除しカウンタ更新を `#ifdef INTERNAL_DIAG` で残す |
| `src/service/ipc_server.cpp` | `#include "../common/win_string_utils.h"` 追加、`CMD_HEALTH_CHECK` JSON dump の catch を同様に2段改善・`LOG_ERROR` 追加 |

### Utf8ToWide バグ修正 (2026/03/02)

**問題**: `cbMultiByte = -1` のとき `MultiByteToWideChar` は null 終端込みの `len` 文字を書き込む。
旧実装は `w(len-1)` でバッファを確保し `cchWideChar = len-1` を渡していたため、
バッファが 1 文字不足して2回目の API 呼び出しが常に 0 を返し、`"(conv_error)"` を返していた。

**修正内容**:

| 変更 | 内容 |
|------|------|
| `w(len-1)` → `w(len)` | null 終端込みの書き込みサイズで確保 |
| `cchWideChar = w.size()` → `len` | バッファ上限を null 終端込みサイズに一致させる |
| `MB_ERR_INVALID_CHARS` 追加 | 不正な UTF-8 シーケンスを検出して `"(conv_error)"` で弾く |
| `w.resize(len - 1)` 追加 | API が書いた null 終端を除去してから return |

**IPC 統計ログ削除の背景**: MEMORY.md 記載方針 (Phase 2 テスト合格後の削除) に従い実施。
`AppendLog` による `[IPC]`/`[IPCQ]` ライブログ出力を完全削除。
カウンタ更新コード自体は `#ifdef INTERNAL_DIAG` で残置（内部診断ビルド向け）。
`lastIpcRefreshTime_` / `ipcRefreshPending_` はリフレッシュ制御に必要なため囲わない。

**検証結果**: Releaseビルド警告ゼロ、71/71 テスト PASS

---

## 8.25 v1.00 リリース前ブラッシュアップ — 完了 (2026/03/02)

**目的**: `/src` 全体の精査（3エージェント並列解析）で判明した実際の問題のみを修正。機能追加・仕様変更なし。

**変更ファイル (8ファイル)**:

| ファイル | Package | 変更内容 |
|---------|---------|---------|
| `src/common/win_string_utils.h` | A-1 | `WideToUtf8(const wchar_t*)` 宣言追加 |
| `src/common/win_string_utils.cpp` | A-1 | UTF-16 → UTF-8 変換実装追加 |
| `src/common/config.cpp` | C-1 | `#include "win_string_utils.h"` 追加、`e.what()` wstring 変換 5箇所を `unleaf::Utf8ToWide(e.what())` に統一 |
| `src/service/ipc_server.cpp` | A-2, C-2 | ローカル `Utf8ToWide`/`WideToUtf8` 2関数を削除し `unleaf::` 名前空間版に置換。`uint32_t` キャストを `memcpy` に変更（アライメント安全化）。`GetModeString` から `DEGRADED_CONFIG` ケース削除 |
| `src/service/engine_core.h` | D-2 | 未使用 `DEGRADED_CONFIG` enum 値を削除 |
| `src/service/engine_core.cpp` | B-2 | `FindNextChangeNotification` 失敗時に `LOG_ALERT` + ハンドルを `INVALID_HANDLE_VALUE` に設定して config 変更検知を安全に無効化 |
| `src/service/process_monitor.cpp` | C-3 | TDH バッファ 512KB 上限追加。`MultiByteToWideChar` 第2呼び出し戻り値チェック追加 |
| `src/manager/main_window.cpp` | B-1, D-1, D-3 | `GetMessage` ループ 3箇所を `> 0` に修正。コメントアウト済み旧実装ブロック 4件（約93行）削除。`CreateSolidBrush` 5箇所に NULL チェック + `LOG_ALERT` 追加 |

**不採用（False Positive として確認済み）**:

| 指摘 | 理由 |
|------|------|
| `scoped_handle.h:64` SC_HANDLE__ | `using pointer = SC_HANDLE` override が正当なパターン |
| `ipc_client.cpp` pipeHandle_ race | ロック内 swap + CloseHandle は設計通り正しく動作 |
| `DeferredVerifyContext` double delete | delete 直後に nullptr 設定 + 全パス確認で二重削除なし |
| `configChangePending_` atomic | EngineControlLoop スレッド単独アクセス設計 |
| `ipc_server.cpp:106` integer overflow | `std::min(..., 4)` で上限設定済み |
| `registry_manager.cpp` RAII | 全パスで CloseHandle 正常 |
| `WaitCallbackContext::process` | UAF 防止の意図的設計 |

**検証結果**: UnLeaf_Service.exe 警告ゼロ、71/71 テスト PASS
（UnLeaf_Manager.exe は実行中のためリンク LNK1104 — コード問題なし）

---

## 8.26 Phase 1 完了 — UnLeaf/ 削除・ダブルバッファリング導入 — 完了 (2026/03/02)

**目的**: リリース前フリッカー対策 + 旧ディレクトリ除去。

### 1-A: `UnLeaf/` ディレクトリ削除

git 未追跡の古い複製ディレクトリ (`UnLeaf/.claudeignore` のみ含む) を削除。
`git status` で残存ファイルなし確認済み。

### 1-B: ダブルバッファリング導入 (`src/manager/main_window.cpp`)

`OnPaint()` をメモリ DC + BitBlt 方式に移行。`ToggleSubclassProc::WM_PAINT` と同パターン。

| 変更 | 内容 |
|------|------|
| メモリ DC 作成 | `CreateCompatibleDC(hdc)` + `CreateCompatibleBitmap(hdc, rc.right, rc.bottom)` |
| 全描画をメモリ DC へ | `FillRect`/`SetBkMode`/`SetTextColor`/`TextOutW` × 2 を `hdcMem` に移行 |
| スクリーンへ転送 | `BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY)` |
| リソース解放 | `SelectObject(hdcMem, hbmOld)` → `DeleteObject(hbmMem)` → `DeleteDC(hdcMem)` |

**スコープ確認**:
- `ToggleSubclassProc::WM_PAINT` — 既にダブルバッファ済み (L1468-1480) → 対象外
- `DrawButton()` (`WM_DRAWITEM`) — システム管理 DC → 対象外
- `OnPaint()` のみが直描画 → 対象

**検証結果**: Release ビルド成功、警告ゼロ

## 8.27 OnPaint() ダブルバッファ安全化 — goto cleanup + 3点強化 — 完了 (2026/03/02)

**目的**: §8.26 で導入したダブルバッファリングに対し、GDI 失敗時のリーク・EndPaint 未呼出・フォント未復元のリスクを排除。

**変更ファイル (1ファイル)**:

| ファイル | 変更内容 |
|---------|---------|
| `src/manager/main_window.cpp` | `OnPaint()` 全面改修 (下記) |

**実装詳細**:

| 項目 | 変更前 | 変更後 |
|------|--------|--------|
| 制御フロー | 直列実行（失敗時にリーク） | `goto cleanup` 単一出口パターン |
| 変数宣言 | 各行でインライン初期化 | `goto` より前に全変数宣言 (C++ jump-over-init 回避) |
| HFONT 復元 | なし | `oldFont = (HFONT)SelectObject(...)` → `cleanup` で復元 |
| Bitmap サイズ | `rc.right, rc.bottom` | `rc.right - rc.left, rc.bottom - rc.top` (差分計算) |
| BitBlt 失敗 | 検知なし | `if (!BitBlt(...)) OutputDebugStringW(...)` |
| cleanup 順序 | SelectObject → Delete → DeleteDC → EndPaint | oldFont 復元 → hbmOld 復元 → DeleteObject → DeleteDC → EndPaint |

**EndPaint 保証**: 全経路 (`goto cleanup` × 3 + 正常終了) で EndPaint が呼ばれる。

**検証結果**: Release ビルド成功、警告ゼロ

---

## 8.28 ResolveRoleColor RGB 重複解消 + 色メンバ追加 — 完了 (2026/03/02)

**目的**: `ResolveRoleColor()` がハードコード RGB を使用していたため、`clr*_` 初期化値の変更が反映されない不整合を解消。`clrPending_` / `clrNotInstalled_` を新規追加し、全 RGB リテラルをメンバ参照に統一。

**変更ファイル (2ファイル)**:

| ファイル | 変更内容 |
|---------|---------|
| `src/manager/main_window.h` | `clrPending_`・`clrNotInstalled_` メンバ追加（コメント付き） |
| `src/manager/main_window.cpp` | 初期化2行追加、`ResolveRoleColor()` 6箇所修正、デッドコード2行削除 |

**ResolveRoleColor 修正内容**:

| 行 (変更前) | 変更前 | 変更後 |
|------------|--------|--------|
| ServiceState::Running | `RGB(46, 204, 113)` | `clrOnline_` |
| ServiceState::StopPending | `RGB(255, 215, 0)` | `clrPending_` |
| ServiceState::default | `RGB(231, 76, 60)` | `clrTextDim_` (未知状態を Offline と誤認しないため) |
| Diagnostic::Running | `RGB(46, 204, 113)` | `clrOnline_` |
| Diagnostic::Stopped | `RGB(231, 76, 60)` | `clrOffline_` |
| Diagnostic::StopPending | `RGB(255, 215, 0)` | `clrPending_` |
| Diagnostic::NotInstalled | `RGB(140, 140, 150)` | `clrNotInstalled_` |

**追加メンバ**:

```cpp
COLORREF clrPending_;       // Gold: visually distinct from Running(Green) and Stopped(Red)
COLORREF clrNotInstalled_;  // Slightly darker than clrTextDim_ (absence vs inactivity)
```

**デッドコード削除**: `UpdateServiceStatus()` 末尾のコメントアウト2行 (旧 `InvalidateRect` 部分範囲更新コード) を削除。

**効果**: `ResolveRoleColor()` 内の RGB リテラルが完全にゼロ。テーマ変更時の修正漏れリスクを排除。

**検証結果**: Release ビルド成功、警告ゼロ

---

### §8.29 UI_RULES.md 新規作成 (2026/03/02)

**経緯**: `docs/UI_RULES.md` は §8.18 で「作成済み」と記録されていたがディスク上に存在しなかったため、
v1.00 実施項目 #7 として正式に新規作成。

**作成ファイル**: `docs/UI_RULES.md`

**収録ルール**:

| 章 | 内容 |
|----|------|
| 1. 設計原則 | ダブルバッファ・OS描画禁止・レイアウト一元化・ロールベース色管理 |
| 2. 描画規約 | WM_ERASEBKGND return TRUE のみ / OnPaint メモリDC / FillRect / GetSysColor禁止 |
| 3. 色管理規約 | RGB リテラル禁止 / clr*_ 管理 / ResolveRoleColor 唯一経路 / WM_CTLCOLOR* ブラシ固定 |
| 4. レイアウト規約 | ComputeLayoutMetrics/RepositionControls 唯一入口 / DpiScale() 必須 / 差分座標のみ |
| 5. GDI リソース管理 | goto cleanup 方式 / SelectObject 復元 / EndPaint 必須 / BitBlt 失敗検知 |

各ルールは `Rule: / Reason: / Enforcement:` の3項目形式で記述。

**v1.00 ロードマップ #1〜#7 すべて完了。**

---

## 10. 今後の改修計画 (Future Roadmap)

### ChatGPT 提案のレビュー判定

#### Phase 1: 描画基盤固定

| # | 提案 | 判定 | 理由 |
|---|------|------|------|
| 1 | 描画責任の統一 | **実装済み** | §8.18で完了。`WM_ERASEBKGND→return TRUE`、`OnPaint()`冒頭で`FillRect`、`GetSysColor`全廃、ダークパレット固定。`docs/UI_RULES.md`で明文化済み |
| 2 | 子コントロール背景統制 | **実装済み** | `WM_CTLCOLORSTATIC`/`Edit`/`Listbox`全てアプリブラシ返却済み。`ControlRole`ベースの色解決も導入済み (§8.16) |
| 3 | フリッカー防止（ダブルバッファリング） | **v1.00で実施** | UI_RULES.mdで「将来対応前提」と記載済み。描画責任一本化が完了した今が導入適期 |

#### Phase 2: UI構造整理

| # | 提案 | 判定 | 理由 |
|---|------|------|------|
| 4 | LayoutManagerクラス導入 | **却下** | 現在2557行の単一ウィンドウアプリに対してオーバーエンジニアリング。`ComputeLayoutMetrics()`+`RepositionControls()`+`DpiScale()`で十分機能しており、Anchor/Margin抽象化の利益がコスト（新クラス＋全コントロール移行）に見合わない。ウィンドウが1つしかないため再利用性もない |
| 5 | FontManager導入 | **実装済み** | §8.16で`FontSet`構造体+`RecreateFontsForDpi()`を導入済み。`FontEntry`(handle+isStock+Reset())による安全なライフタイム管理、DPI変更時の一括再生成も完了。別クラスに分離する必要はない（MainWindowからしか使わない） |
| 6 | UIコンポーネントのクラス化 | **却下** | ToggleButton/StatusCard/LogViewの分離は、Win32ネイティブAPIの制約上、メッセージループとの結合が密すぎてクリーンな分離が困難。現在の`DrawToggleSwitch()`/`DrawButton()`/`DrawStatusIndicator()`はprivateメソッドとして十分に責務分離されている。WPFやImGuiなら妥当だがWin32では実装コストが利益を大幅に上回る |

#### Phase 3: 状態管理分離

| # | 提案 | 判定 | 理由 |
|---|------|------|------|
| 7 | AppState構造体導入 | **却下** | 現在の状態は`ServiceController::GetServiceState()`（外部問い合わせ）と`IPCClient`（ヘルスチェック）で取得し、`currentState_`/`activeProcessCount_`のatomicメンバで保持。UIは`UpdateServiceStatus()`/`UpdateEngineStatus()`のタイマーコールバック経由で参照のみ。既に「UIはポーリング取得した状態を表示するだけ」の設計になっており、AppState抽象化は名前の変更に過ぎない |
| 8 | ログ描画非同期化 | **実装済み** | §8.9で完了。`LogWatcherThread`→`pendingLogLines_`(CriticalSection保護)→`PostMessage(WM_LOG_REFRESH)`→`RefreshLogDisplay()`差分追記。UIスレッドは描画のみ。workerで生成。まさにChatGPTが提案した構成が既に動作中 |

#### Phase 4: プロダクト基盤

| # | 提案 | 判定 | 理由 |
|---|------|------|------|
| 9 | 設定保存（Window位置等） | **将来バージョン** | 機能追加であり「基盤固定フェーズ」の範囲外。v1.01以降で検討 |
| 10 | エラーハンドリング統一 | **v1.00で一部実施** | `catch(...)`→`catch(std::exception&)`改善は妥当。ErrorCode enumは過剰（エラー種別が少ない） |

#### 「重要ルール」への見解

| ルール | 判定 | 理由 |
|--------|------|------|
| UIロジックをWindowProcへ書かない | **不適切** | Win32ネイティブアプリでは`WndProc`/`HandleMessage`がUIロジックの正統な場所。MVVMやMVCのフレームワークがない環境でこれを禁止すると、メッセージのディスパッチ→呼び出し→再ディスパッチの無意味な間接化が発生する |
| 描画コードは必ずUIクラスへ分離 | **過剰** | 既にprivateメソッド(`OnPaint`/`DrawButton`/`DrawToggleSwitch`/`DrawStatusIndicator`)に分離済み。別クラスへの分離はWin32では実装コスト過大 |
| 色・フォント・レイアウト値のハードコード禁止 | **一部妥当** | 色定数の集約は実施すべき（RGB重複32箇所）。フォント/レイアウトは`FontSet`+`ComputeLayoutMetrics()`で既にハードコード排除済み |
| すべてManagerクラス経由で取得 | **却下** | 単一ウィンドウアプリにManager/Factory/Providerパターンを適用するのはアーキテクチャ宇宙飛行士的アプローチ |

### 統合改修計画

#### v1.00 実施項目（優先順）

| # | 項目 | 対象ファイル | 内容 |
|---|------|-------------|------|
| 1 | **ダブルバッファリング導入** | `main_window.cpp` | **完了 §8.26** `OnPaint()`でメモリDC+`BitBlt`。現在の`FillRect`+`TextOut`直描画をメモリDCに移行。UI_RULES.mdで宣言済みの将来対応を実施 |
| 2 | **ResolveRoleColor RGB 重複解消 + 色メンバ追加** | `main_window.h/cpp` | **完了 §8.28** `clrPending_`/`clrNotInstalled_` 追加。`ResolveRoleColor()` 内の全 RGB リテラルをメンバ参照に統一。ServiceState::default を `clrOffline_` → `clrTextDim_` に変更（誤警告防止）。ThemePalette 構造体への集約は v1.01+ バックログへ移動 |
| 3 | **デッドコード除去** | `main_window.cpp` | **完了 §8.28** `UpdateServiceStatus()` 末尾コメントアウト2行削除。旧 `WM_CTLCOLORSTATIC` ブロックは §8.25 で既に削除済み |
| 4 | **例外ハンドリング改善** | `main_window.cpp`, `ipc_server.cpp` | **完了 §8.24** `catch(...)`→`catch(std::exception&)`+`catch(...)`2段化。`Utf8ToWide(e.what())`でUTF-8安全ログ出力 |
| 5 | **`UnLeaf/` ディレクトリ削除** | プロジェクトルート | **完了 §8.26** git未追跡の古い複製。混乱源 |
| 6 | **CMakeLists.txt 清掃** | `CMakeLists.txt` | **完了 §8.24** コメントアウト済み旧RCファイル参照削除 |
| 7 | **レイアウト責務ルール明文化** | `docs/UI_RULES.md` | **完了 §8.29** `docs/UI_RULES.md` 新規作成。5章構成 (設計原則/描画/色/レイアウト/GDI)。Rule/Reason/Enforcement 形式。RECT計算一元化ルール明記 |

#### 将来バージョン（v1.01+）

| # | 項目 | 理由 |
|---|------|------|
| 8 | ThemePalette 構造体への集約 | `clr*_` メンバを構造体に束ねる整理。機能影響なし。§8.28 で RGB 重複は解消済みのため緊急度低 |
| 9 | 設定保存（ウィンドウ位置・状態永続化） | 機能追加。基盤固定後に実施 |
| 9 | engine_core ユニットテスト | テスタブル関数の抽出が先に必要。大規模リファクタ |
| 10 | IPC プロトコル仕様書 | ドキュメント追加。実装に影響なし |
| 11 | CI/CD 構築 | 既存バックログ (§10.4)。GitHub Actions |
| 12 | Manager ロギング統一 | `AppendLog`→共通ロガー経由。影響範囲が広い |

#### 却下項目

| # | 項目 | 理由 |
|---|------|------|
| - | LayoutManagerクラス | 単一ウィンドウに対してオーバーエンジニアリング |
| - | UIコンポーネントのクラス化 | Win32ネイティブでは実装コスト過大、利益薄 |
| - | AppState構造体 | 既存設計で実質同じ責務分離が達成済み |
| - | ErrorCode enum | エラー種別が少なく、enum化の利益がない |
| - | 「全てManagerクラス経由」ルール | 抽象化過剰。単一ウィンドウアプリに不適 |

### 10.2 品質改善 (Quality) — 完了項目

| 項目 | 対象ファイル | 詳細 |
|------|-------------|------|
| 未使用フィールド整理 | engine_core.h/cpp | `stableCheckInterval`, `isTrustedStable` 削除済 (2026/02/14) |
| バージョンコメント整理 | 全15ソース | `// v5.0:` 〜 `// v8.0:`, `// RC-Pn:` プレフィックス完全除去 (2026/02/14) |

### 10.4 技術的改善 (Tech Debt) — 完了項目

| 項目 | 詳細 |
|------|------|
| Initialize() ハンドルリーク | `CleanupHandles()` ヘルパー導入。Initialize() 失敗パス 6箇所 + Stop() Step 9 で使用。`stopEvent_` リーク修正含む (2026/02/15) |
| ユニットテスト導入 | GoogleTest 71/71 PASS (§8.4 参照) |
| `/W4` 警告レベル引き上げ | `/W3`→`/W4` 昇格。C4324(pragma抑制)、C4244(tolower lambda化)、C4505(未使用関数削除) 修正。警告0ビルド達成 (2026/02/15) |
| 静的解析ツール導入 | `/W4` 警告ゼロビルドで代替。MSVC `/W4` が実質的な静的解析として機能し、外部ツール不要と判断 (2026/02/15) |
| nlohmann/json 導入 | 手書き ostringstream JSON → nlohmann/json v3.11.3 (FetchContent)。CMD_HEALTH_CHECK 完全移行 (2026/02/20) |

---

## 10.5 ハンドルリーク調査 — Manager 側分離計測計画 (改訂版, 2026/02/27)

### 背景

- PDH 11.5h 計測でハンドル数が +39 増加 (§8.6 参照)
- §8.6 でゾンビ TrackedProcess リークは修正済み
- **Service 側 Working Set 増加 (§8.22) は別件として解決済み** (2026/02/27)
  - `errorLogSuppression_` / `policyAppliedSet_` コンテナの無制限蓄積が原因
  - TTL クリーンアップ + サイズ上限で対処完了
- **Service 側 waitHandle リークは Phase 1 テストで否定済み** (HandleCount が横ばいであることを確認)
- 次の調査対象: **Manager (`UnLeaf_Manager.exe`) 側**
- `HandleCount` 単独では Kernel/GDI/USER が混在し誤認リスク → 分離計測・Pipe個別確認・IPC観測を追加

### 静的解析サマリ (コード変更なし)

| 調査対象 | 結果 |
|---------|------|
| `GetServiceState()` (1秒毎) | `ScopedSCMHandle` RAII — 全パスで自動 `CloseServiceHandle` ✅ |
| `ipcClient_.SendCommand()` | 毎トランザクション後 `CloseHandle(pipeHandle_)` — ステートレスパターン ✅ |
| `LogWatcherThread` ログファイル | 500ms 毎に open/close — 全パスで `CloseHandle` ✅ |
| `logWakeEvent_` | デストラクタで `CloseHandle` ✅ |
| GDI ハンドル (ブラシ/フォント) | デストラクタで `DeleteObject` / `FreeLibrary` ✅ |
| hMutex | `main.cpp` で `ReleaseMutex` + `CloseHandle` ✅ |

**設計上の注意点 (リークではないが観測対象)**:
- `UpdateEngineStatus()` の二重呼び出しは §8.23 で修正済み（WM_IPC_REFRESH に移動、3 秒レートリミット）

---

### Phase A — Manager 3値分離計測

**ターミナル1 — Manager 3値分離計測**:

```powershell
while ($true) {
    $p = Get-Process -Name UnLeaf_Manager -ErrorAction SilentlyContinue
    if ($p) {
        "$((Get-Date).ToString('HH:mm:ss'))  handles=$($p.HandleCount)  GDI=$($p.GDIObjects)  USER=$($p.UserObjects)  mem=$([int]($p.WorkingSet64/1KB))KB"
    }
    Start-Sleep 60
}
```

**ターミナル2 — Service/Manager 同時比較**:

```powershell
while ($true) {
    $s = Get-Process -Name UnLeaf_Service -ErrorAction SilentlyContinue
    $m = Get-Process -Name UnLeaf_Manager -ErrorAction SilentlyContinue
    $t = (Get-Date).ToString('HH:mm:ss')
    if ($s -and $m) {
        "${t}  Service: handles=$($s.HandleCount)  Manager: handles=$($m.HandleCount) GDI=$($m.GDIObjects) USER=$($m.UserObjects)"
    }
    Start-Sleep 60
}
```

計測時間: **最低 2 時間** (Chrome を通常使用しながら放置)

### Phase A 判定基準

| 増加する値 | 疑い | 次のアクション |
|-----------|------|--------------|
| `handles` のみ増加 | Kernel ハンドルリーク | Phase B-1 (Process Explorer Handle Type ソート) |
| `GDI` 増加 | ブラシ/フォント/DC の DeleteObject 漏れ | `main_window.cpp` デストラクタを再確認 |
| `USER` 増加 | ウィンドウ/メニュー/トレイアイコン | `DestroyWindow` / `DestroyMenu` パスを確認 |
| 全て横ばい | Manager 側リークなし | ETW/OS 内部/計測誤差の調査へ |

---

### Phase B — Process Explorer による Handle Type 別特定 + Pipe 個別確認

**前提**: Phase A で Manager handles の単調増加が確認された場合のみ実施。

**B-1: Handle Type ソート**
1. Process Explorer を管理者として起動
2. `UnLeaf_Manager.exe` をダブルクリック → **Handles タブ**
3. **Type 列でソート** — 増加している型を特定する
4. 30分おきにスナップショットを記録

| Type | 増加している場合の候補原因 |
|------|--------------------------|
| `Pipe` | `pipeHandle_` の Disconnect 漏れ |
| `File` | LogWatcherThread の CloseHandle 漏れ (L2416) |
| `Event` | 予期しない CreateEvent 積み重なり |
| `SCM Service` | `ScopedSCMHandle` のデストラクタが動いていない |
| `Section` | FreeLibrary 漏れ |
| `Key` | RegOpenKey 系の呼び出し漏れ |

**B-2: NamedPipe リーク個別確認**

```
Process Explorer
  → メニュー: Find → "Find Handle or DLL..."
  → 検索文字列: \Device\NamedPipe
  → 検索実行 → 結果件数を記録
  → 30分後に再検索 → 件数が増えていないか確認
```

| 結果 | 判断 |
|------|------|
| 件数が横ばい | パイプリークなし |
| `\Device\NamedPipe\UnLeafServicePipe` のエントリが増加 | `SendCommand()` の Disconnect 漏れ確定 → `ipc_client.cpp` L66-73 を確認 |
| 別パスのエントリが増加 | 未知の Pipe 生成元を確認 |

---

### Phase C — UpdateEngineStatus 二重呼び出しの観測

**C-1: CPU/ハンドルへの影響計測 (別ターミナル、30秒間隔)**:

```powershell
while ($true) {
    $m = Get-Process -Name UnLeaf_Manager -ErrorAction SilentlyContinue
    if ($m) {
        "$((Get-Date).ToString('HH:mm:ss'))  CPU=$([math]::Round($m.CPU,1))s  handles=$($m.HandleCount)  GDI=$($m.GDIObjects)"
    }
    Start-Sleep 30
}
```

CPU 使用時間が継続増加 → 7200回/時 IPC が負荷になっている可能性 → IPC タイムスタンプ観測ログ実装を検討

**C-2: IPC タイムスタンプ観測案 (将来参考 — 現時点では未実装)**:

> コード変更は別途承認を要する。調査結果次第で実装を判断する。

`ipc_client.cpp` の `SendCommand()` 前後に追加する観測ログ案:

```cpp
// [IPC] start <GetTickCount64()>
// ... 既存の SendCommand ロジック ...
// [IPC] end <GetTickCount64()>  elapsed=<ms>
```

| 観測値 | 意味 |
|--------|------|
| `elapsed > 100ms` | パイプ接続の遅延 or サービス側が詰まっている |
| 2回の `start` が 1ms 以内に連続 | UpdateEngineStatus 二重呼び出しの確認 |
| `elapsed` が時間とともに増加 | パイプ競合またはサービス側の蓄積 |

---

### 全体の判定フロー

```
Phase A: Manager を 2h 計測 (handles / GDI / USER 分離)

handles のみ増加?
  YES → Phase B-1: Process Explorer で Type ソート
        → Type = Pipe?
            YES → Phase B-2: \Device\NamedPipe 検索で確定
        → Type = File? → LogWatcherThread L2416 の CloseHandle パスを確認
        → Type = Event? → 未知の CreateEvent を grep で探す
        → Type = SCM Service? → ScopedSCMHandle のデストラクタ動作を確認

GDI 増加? → main_window.cpp デストラクタの DeleteObject パスを確認
USER 増加? → DestroyWindow / DestroyMenu / Shell_NotifyIconW(NIM_DELETE) パスを確認
全横ばい? → Manager 側リークなし → ETW/OS 内部ハンドル調査 (別 Phase)

CPU 増加 (Phase C)? → IPC タイムスタンプ観測ログの実装を検討
```

### Phase A 計測結果 (2026/02/27 — 3h43m)

| 指標 | 開始値 | 終了値 | 変化 | 判定 |
|------|--------|--------|------|------|
| handles | 224/226 | 224/226 | ±0 (2値交互) | ✅ リークなし |
| GDI | 48 | 48 | +0 (全期間固定) | ✅ クリーン |
| USER | 43 | 43 | +0 (全期間固定) | ✅ クリーン |
| mem | 24,636 KB | 25,212 KB | +576 KB | △ 正常範囲 |

**handles 2値交互の解釈**: `UpdateEngineStatus()` 二重呼び出しによるパイプ open/close のタイミング差。単調増加なし → Kernel ハンドルリークなし確定。

**mem +576 KB**: §8.6 修正前 (211 KB/h) より低く (154 KB/h)、かつ 14:06-07 で一時減少。RichEdit ログバッファ自然増加 + Working Set ページング変動の組み合わせによる正常範囲内の増加と判定。

**最末行 USER=45**: Terminal1 (14:07:23) は 43 のまま。Terminal2 (14:07:45、22秒後) のみ 45。操作による瞬間値。継続増加なし → USER リークなし。

### 最終判定: **Manager 側リークなし — 確定 ✅**

```
handles のみ増加? → NO    GDI 増加? → NO    USER 増加? → NO    全横ばい → YES
```

**ハンドルリーク調査 全体結論:**
| 調査フェーズ | 対象 | 結果 |
|------------|------|------|
| §8.6 | ゾンビ TrackedProcess | **修正済み (根本原因対処)** |
| Service Phase 1 | waitHandle リーク | **否定済み** |
| Manager Phase A | Kernel/GDI/USER リーク | **否定済み** |

→ **§8.6 の修正が根本原因の対処。現在ハンドルリークは解消されている。**

**残存設計注意点 (リークではなく設計改善候補)**:
- `UpdateEngineStatus()` 二重呼び出し → パイプ 7,200 回/時。別タスクとして修正を検討。

### ステータス: **完了 (2026/02/27)**

---
**Claude Code への指示**:
本ファイルは現在の作業状況を補足するための参考情報であり、設計判断は CLAUDE.md を最優先とする。
このファイルを読み込み、環境調査をスキップして作業を再開してください。
作業終了時には、必ずこのファイルを更新してください。
