# Changelog

All notable changes to UnLeaf will be documented in this file.

---

## [1.1.2] - 2026-04-11

### Fixed
- `jobObjects_` entries not erased on process exit, causing Windows Job Object handle accumulation (~9 MB / 41 hours). Added `jobObjects_.erase(pid)` under `jobCs_` at the end of `RemoveTrackedProcess()`, after all wait/timer teardown and outside `trackedCs_` to preserve `jobCs_ → trackedCs_` lock order

### Changed
- `ParseProcessStartEvent`: replaced per-call `std::vector<BYTE>` allocations with `thread_local` reusable buffers (`s_tdhBuffer`, `s_propBuffer`). Capacity grows on demand and never shrinks, eliminating repeated heap alloc/free per ETW event. Added `#ifdef _DEBUG` check for abnormal buffer growth (> `MAX_TDH_BUFFER * 2`)
- `ProcessMonitor` ctor / `Start()` / `Stop()`: replaced `std::wstringstream` with `std::to_wstring()` at 4 log sites to reduce heap fragmentation
- `engineControlThreadId_` changed from `DWORD` to `std::atomic<DWORD>` for cross-thread visibility guarantee. `EngineControlLoop` uses `store(..., relaxed)` at entry and exit. Debug asserts in `RemoveTrackedProcess`, `RefreshJobObjectPids`, `ProcessPendingRemovals` use `load(..., relaxed)` for deterministic misuse detection
- `EngineCore::Start()`: added `HeapSetInformation(GetProcessHeap(), HeapOptimizeResources, ...)` to encourage the process heap to decommit idle pages more aggressively (Windows 8.1+, best-effort). Reduces Private Working Set growth from heap fragmentation over long runs. Verified: ~5x improvement in Private Bytes growth rate (3.78→6.26 MB over 6.5h vs 3→15.5 MB previously)

---

## [1.1.1] - 2026-04-09

### Changed (§9.15 ProcessMonitor 堅牢性強化)
- `IsHealthy()` を 3 段判定に再設計: (1) ウォームアップ猶予 120s (2) lost event デルタ閾値チェック (3) `ControlTraceW(QUERY)` によるセッション生存確認 (1s キャッシュ)。idle (正常な無イベント) と ETW セッション死を区別する
- `instance_` を `std::atomic<ProcessMonitor*>` に変更。`EventRecordCallback` でローカル `self` にスナップショットし TOCTOU を排除。acquire/release ペアリング
- `Stop()` を `stopMtx_` で保護し ETW シャットダウン契約 (5 ステップ順序: `stopRequested_` → `CloseTrace` → `ControlTrace(STOP)` → `join` → `instance_ clear`) を厳密化。IPC スレッドの `IsHealthy()` との race を排除
- `ParseProcessStartEvent` の文字列プロパティ読み取りを `propSize` 上限の bounded copy に置換。非終端 TDH ペイロードによる領域外読み (AV リスク) を排除。未知 `InType` はスキップ
- `Start()` で `eventCount_` / `lostEventCount_` / `lastEventTime_` / `startTime_` / `lastCheckedLost_` / `cachedTraceAlive_` を明示リセット。再起動後の残留状態汚染を防止
- `ControlTraceW(STOP)` の戻り値を検査。`ERROR_SUCCESS` / `ERROR_MORE_DATA` / `ERROR_WMI_INSTANCE_NOT_FOUND` 以外は `LOG_DEBUG` で記録
- `Stop()` 時に `cachedTraceAlive_` / `lastTraceCheckTime_` をクリアし、次回 `Start()` 後の stale "alive" を防止

### Added (CrashDump opt-in — Phase 2)
- `src/common/crash_handler.{h,cpp}` 新設: `SetUnhandledExceptionFilter` + `MiniDumpWriteDump(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory)` ベースの未処理例外ハンドラ。既定無効、`[Logging] CrashDump=1` で opt-in
- 出力先: `<install dir>\crash\UnLeaf_Service_YYYYMMDD_HHMMSS.sss.dmp` (ポータブル方針堅持)
- `UnLeafConfig::IsCrashDumpEnabled()` + `crashDumpEnabled_` メンバ追加
- `SetUnhandledExceptionFilter` の戻り値 (旧フィルタ) を install 時に取得し、非 null のみ `LOG_DEBUG` で記録。crash 時チェインは行わず `EXCEPTION_CONTINUE_SEARCH` で WER に委譲
- `dbghelp.lib` リンク + `/Zi /DEBUG /OPT:REF /OPT:ICF` で PDB 常時出力

---

## [1.1.0] - 2026-03-30

### Changed (§9.00〜§9.09 メモリ安定化・長期稼働品質改善)
- `trackedProcesses_` ハード上限 (`MAX_TRACKED_PROCESSES=2000`) 到達時に `SelectEvictionCandidate()` で zombie 優先・最古優先の退避候補を選出し `pendingRemovalPids_` 経由で `RemoveTrackedProcess()` に委任 — Timer/WaitContext リークを防止 (§9.00)
- eviction 内の `trackedProcesses_.erase` / `waitContexts_.erase` を削除 — `RemoveTrackedProcess()` 一元管理に統一、TrackedCs_ 保持中の erase 禁止 (§9.02)
- `evictionCounter` 無条件インクリメント + `forceEvict` (size ≥ MAX+32 で全 excess を一括退避)、burst drain は `SelectEvictionCandidates()` で N 件を1パス収集 (§9.03/§9.07/§9.08)
- `pendingRemovalPids_` saturation 時の `pop()` を廃止 — `LOG_ALERT` のみ出力し push を継続（eviction 作業ドロップ禁止）(§9.06/§9.09)
- `ProcessPendingRemovals()` に `MAX_DRAIN_PER_TICK=256` 制限を追加 — tick 毎の処理量を平準化、残留時は `SetEvent` で次 tick を自動スケジュール、backlog > 8192 で `LOG_ALERT` (§9.09)
- `ProcessEnforcementQueue()` / `HandleSafetyNetCheck()` の PMR arena を `#ifdef _DEBUG` ガードから常時有効化 — Release でも PMR fallback を `LOG_INFO` で検出 (§9.01/§9.05)
- `SelectEvictionCandidates()` 内部一時変数 (`picked`・`aged`) を PMR arena (16KB stack buffer) に移行、`partial_sort` 最適化、safety cap 追加 (§9.08/§9.09)
- `Logger::WriteMessage()` をスタックバッファ (2048 bytes) で書き込み — 通常パスのヒープ割当ゼロ (§9.00)

### Added (§9.00〜§9.09)
- `CountingResource` / `IsCSHeldByCurrent` / `SelectEvictionCandidates` を anonymous namespace に一元定義 — ODR 違反なし、ヘッダ変更不要 (§9.05/§9.07)
- `SelectEvictionCandidate()` 先頭に DEBUG ASSERT (`IsCSHeldByCurrent`) 追加 — `trackedCs_` 保持義務を実行時検証 (§9.07)
- `pendingRemovalPids_` backlog の3段階ログ: 50% → `LOG_INFO`、75% → `LOG_ALERT`、overflow → `LOG_ALERT` (§9.04/§9.09)

### Changed (§9.10 RegistryPolicyManager v5)
- RegistryPolicyManager を全面再設計: IFEO (exe 名単位) と PowerThrottle (パス単位) を分離管理。per-entry state machine (APPLYING→COMMITTED)、lock-free Treiber stack、`policyCs_` ZERO I/O、`ifeoRefCount_` 参照カウント
- マニフェスト形式を v5 に更新: `[IFEOPolicies]` + `[PowerThrottlePolicies]` の2セクション構成
- `SaveManifestAtomic()`: temp file + `FlushFileBuffers` + `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` で原子的更新。`stateVersion_` による snapshot 整合性チェック付き
- `VerifyAndRepair()` (30分間隔): registry/memory 不整合を自動修復
- `IsCanonicalPathImpl` + `UNLEAF_ASSERT_CANONICAL` マクロで正規化済みパスを実行時検証
- LRU キャッシュ (list + unordered_map) で EngineCore 側の重複 ApplyPolicy を排除

### Fixed (§9.11 CPU 暴走対策)
- `ProcessPendingRemovals` の無条件 `SetEvent(hWakeupEvent_)` による CPU 96.9% 暴走を修正。`hasRemaining` フラグを lock 内で取得し、SetEvent を lock 外で条件付き実行
- 全 push サイト (P1: OnProcessStop, P2: Zombie detection, P3: Eviction) を `wasEmpty` パターンに統一
- EngineControlLoop にスピン検知を追加 (連続 10,000 wakeup 超で `[SPIN DETECTED]` ログ + 1ms sleep)

### Added (§9.12 プロアクティブポリシー生成)
- `ApplyProactivePolicies()`: Initialize / HandleConfigChange 時に config 全エントリへポリシー適用。リアクティブ→プロアクティブへアーキテクチャ移行
- `ApplyIFEOOnly()`: name-only target の IFEO プロアクティブ適用
- `ReconcileWithConfig()`: config から除外されたエントリの自動削除。`stateVersion_` CAS による race condition 防止
- `HasPolicy()` + ETW フォールバック: `ApplyOptimizationWithHandle` でプロアクティブ適用失敗時に `ApplyPolicy` をリカバリ呼び出し
- `FileExistsW()`: パス存在確認。存在時は IFEO + PowerThrottle、不在時は IFEO のみ適用

### Fixed (§9.14 name-only ターゲット PowerThrottle 遅延適用バグ)
- `ApplyOptimizationWithHandle` に name-only / path-based 分岐を追加: `targetNameSet_` に登録された name-only ターゲット (例: `chrome.exe=1`) は `HasPolicy()` ゲートをバイパスし、resolvedPath が取得できた時点で即座に `ApplyPolicy()` を呼び出す。これによりサービス起動後に起動したプロセスに PowerThrottle レジストリポリシーが適用されない問題を解消
- `TrackedProcess` に `needsPolicyRetry` フラグを追加: ETW コールバック時点で `QueryFullProcessImageNameW` が失敗 (イメージマッピング未完了) した場合に `true` を設定
- SafetyNet (10s) にポリシー回復ロジックを追加: `needsPolicyRetry = true` のプロセスに対し ResolveProcessPath を再試行し、パス取得成功時に `ApplyPolicy` を呼び出して `fullPath` と `needsPolicyRetry` を更新
- SafetyNet PMR arena を 4 KB → 8 KB に拡張 (PolicyRetryInfo ベクタ分を確保)
- 診断ログ追加: `ResolveProcessPath` 失敗時 / `ApplyOptimization` パス解決失敗時 / `ApplyOptimizationWithHandle` 空パス遅延ログ (`[DIAG]` タグ)

### Changed (§9.13 CanonicalizePath 正規化統一)
- `ResolvePathByHandle` を完全廃止し `CanonicalizePath` (`GetFullPathNameW` ベース、ファイル存在不要) に置換
- `CanonicalizePath` を `types.h` に free function として配置 (EngineCore・RegistryPolicyManager 双方から利用)
- `GetFullPathNameW` 2段階呼び出し (動的バッファ) で MAX_PATH 制限を撤廃
- `policyMap_` の全キー生成・検索を `CanonicalizePath` に統一。`NormalizePath` はフォールバック/ログ用途に限定
- 設計契約コメント (DESIGN CONTRACT / DO NOT) を `engine_core.cpp`, `registry_manager.cpp`, `registry_manager.h` に追加

### Fixed/Added (§9.14 Rev.17 ServiceEngine メモリ増加対策)
- `enforcementQueue_` (std::queue) を `criticalQueue_` + `nonCriticalQueue_` (各 std::deque) に分離。CRITICAL = ETW_THREAD_START 以外の全型; NON-CRITICAL = ETW_THREAD_START (SOFT_LIMIT=4096 でドロップ可)。TOTAL_LIMIT=8192 絶対保証を static_assert で強制
- nonCritical 空 + TOTAL_LIMIT 到達時の CRITICAL-to-CRITICAL eviction 追加 (全喪失より最古破棄を優先)
- `ENFORCEMENT_CRITICAL_PER_TICK=512` バースト制限: 1 tick 最大 512 件の CRITICAL 処理; 残件は次 tick へ継続
- `ScheduleDeferredVerification` 修正: `std::exchange` + `INVALID_HANDLE_VALUE` 同期待機で旧タイマーハンドル・コンテキストリークを根絶
- `EnqueuePendingRemoval` を CAS ベース `pendingQueueSize_` 上限ガード (MAX=512) に置換。overflow 時: `pendingOverflowFlag_` をセットしノードを delete (サイレントリークなし)
- `DrainPendingRemovals` を RAII `NodeGuard` (インライン struct) に置換: `~NodeGuard() noexcept` がスコープ終了時に `fetch_sub + delete` を保証。re-enqueue ループを廃止 — PendingRemoval 線形増大の主因を排除
- `ConsumePendingOverflowFlag()` + `HandleSafetyNetCheck` 内即時 `VerifyAndRepair` パスを追加 — overflow リカバリを ≤10秒で保証
- `ScanRunningProcessesForMissedTargets(maxScan)` 追加: `std::max` 単調増加保証付き `lastScannedPid_` による2パスラウンドロビン。スナップショット順序変動による永続的な枯渇を防止
- SafetyNet デュアルトリガー追加: CRITICAL drop 差分検出 + 30秒バックストップ (`SAFETY_SCAN_BACKSTOP_MS`) — ETW カーネルレベルのサイレントドロップにも対応
- `ReconcileWithConfig` 先頭で `ifeoRefCount_` を `policyMap_` から完全再構築 — config リロード間の参照カウントドリフトを防止
- `trackedProcesses_` 退避ロジック簡略化: 上限到達時は常に最大 16 件/挿入を選出して即処理 (period-skew カウンタを削除)
- insert 直後に `errorLogSuppression_` サイズを即キャップ (SUPPRESSION_MAX_SIZE/2) — TTL クリーンアップ間のマップ増大を抑制
- `PerformPeriodicMaintenance` に 60秒 `[DIAG] crit= nc= pending= drop= critDrop= critEvict= recovered= tracked=` 診断ログを追加

---

## [1.0.3] - 2026-03-25

### Fixed
- Log rotation 2nd-cycle crash: `CheckRotation()` called `Log()` internally, causing `Log() → WriteMessage() → CheckRotation() → Log()` infinite recursion → stack overflow when rotation failed. Resolved by making `CheckRotation()` a pure function returning `RotationResult`; all diagnostic output routed through `SafeInternalLog()` which writes directly via `WriteFile` without re-entering `CheckRotation()`
- Log rotation 2nd-cycle rename failure: `MoveFileExW(REPLACE_EXISTING)` fails when Manager holds an open handle to `UnLeaf.log.1` — Windows NTFS marks the destination for deletion but keeps the directory entry occupied until all handles close. Replaced with `SetFileInformationByHandle(FileRenameInfoEx, FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS)` for atomic directory-entry replacement regardless of open handles
- `FlushFileBuffers` failure now aborts rotation and closes `fileHandle_` — prevents infinite retry spin on the next write cycle
- `WriteFile` failure in `WriteMessage()` now disables further logging (`enabled_ = false`) to prevent I/O error loops
- Log sequence reversal after rotation: Manager's stale handle check ran **after** `WriteFile`, so the first write post-rotation landed in the already-renamed `UnLeaf.log.1`, making its timestamp newer than the first line of `UnLeaf.log`. Fixed by moving the stale check to **before** `WriteFile` in `WriteMessage()`; added `Global\UnLeafLogRotated` named event (auto-reset) so Service signals Manager immediately on successful rotation rather than waiting up to 100 writes for the counter fallback

### Added
- `RotationResult` struct: `CheckRotation()` returns `{ triggered, success, mutexFailed, error }` — no logging inside; caller decides action
- `SafeInternalLog()`: writes rotation meta-messages directly to `fileHandle_` without rotation check or callback; acquires `cs_` internally; falls back to `OPEN_ALWAYS` if handle is `INVALID`
- `RotationMutexGuard` RAII: acquires `Global\UnLeafLogRotation` on construction, releases on destruction — eliminates manual `ReleaseMutex` on all return paths
- `ScopedHandle` for rename handle: `hRename` is `ScopedHandle` via `MakeScopedHandle()` — handle leak impossible on any return path
- `GetLastError()` captured immediately after each API failure into a local variable — prevents value corruption by intervening calls
- On rename failure, `fileHandle_` restored immediately in `CheckRotation()` to avoid `OPEN_ALWAYS` churn on every subsequent write
- `hRotationEvent_` (`Global\UnLeafLogRotated`, auto-reset): Service sets on successful rotation; Manager polls with `WaitForSingleObject(..., 0)` before each write to detect stale handles immediately

---

## [1.0.2] - 2026-03-16

### Fixed
- ETW buffer configuration made explicit: `BufferSize=64KB`, `MinimumBuffers=4`, `MaximumBuffers=32`, `FlushTimer=0` (real-time consumer mode)
- ETW zombie session cleanup: stale sessions with the same name are stopped before `StartTraceW` to prevent `ERROR_ALREADY_EXISTS`
- `RefreshLogDisplay`: initial `endPos` now obtained via `EM_GETTEXTLENGTHEX(GTL_NUMCHARS)` instead of `GetWindowTextLengthW` — eliminates per-line position skew caused by `\r\n` expansion

### Added (2026-03-16)
- ETW lost event detection: `EVENT_TRACE_TYPE_LOST_EVENT` opcode handled in callback; LOG_ALERT emitted with cumulative count on buffer overflow
- Window position and size persistence: saved to `[Manager]` section of `UnLeaf.ini` on `WM_DESTROY`, restored on startup with off-screen guard (`MonitorFromPoint(MONITOR_DEFAULTTONULL)`)
- Virtual ListView live log display: RichEdit replaced with a custom virtual ListView + Owner-Draw renderer; eliminates auto-scroll stall and blank-line artifacts under high log volume
- `LogEngine` / `LogQueue`: new log management infrastructure in `src/manager/`; `LogQueue` accumulates log lines with thread-safe wake-up; `LogEngine` drives the log thread and dispatches lines to both the file logger and the UI via `UICallback` registration — resolves [LIVE-2] Manager operation log persistence

---

## [1.0.1] - 2026-03-09

### Fixed
- `DeleteTimerQueueTimer` moved before `trackedCs_` acquisition to eliminate lock-held invocation (all 5 sites)
- `DrawButton` GDI object restore fixed: old font now explicitly restored via `SelectObject`
- `ToggleSubclassProc` null check added
- Windows version log display corrected: Windows 11 now shows `Windows 11 (Build XXXXX)` instead of `Windows 10.0 (Build XXXXX)` — hybrid detection (build threshold `>= 22000` for major=10, major fallback for Windows 12+)

### Added
- Engine decision logic extracted to `src/engine/engine_logic` (pure C++, no Win32 dependency)
- `EnginePolicy` struct introduced to centralize timing constants, decoupled from `engine_core`
- Unit tests expanded: 72 → 104 cases (`test_engine_logic`: 32, `test_engine_policy`: 2 added)
- GitHub Actions CI: automatic build + ctest on push / pull_request
- CI cache path optimized to `build/_deps` (FetchContent dependencies only, improved hit rate)
- `DrawToggleSwitch()` GDI+ initialization replaced with `GdipGuard` RAII struct — ensures `GdiplusShutdown()` on process exit

---

## [1.0.0] - 2026-03-06

Initial release.

### Features
- Event-driven EcoQoS optimizer using ETW (Event Tracing for Windows)
- 3-phase adaptive control: AGGRESSIVE → STABLE → PERSISTENT
- 5-layer EcoQoS policy enforcement (NT API + Win32 API + Registry)
- Named Pipe IPC with DACL (SYSTEM + Administrators only)
- Win32 GUI Manager with dark theme (GDI+)
- 72 unit tests (GoogleTest)
- Zero CPU usage at idle (`WaitForMultipleObjects(INFINITE)`)
- Memory footprint: ~15 MB
