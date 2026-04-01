# UnLeaf v1.1.0 Project Context

> **最終更新**: 2026-03-30
> **ステータス**: **v1.1.0 開発中** — プロアクティブポリシー生成 + CanonicalizePath 正規化統一 + 設計契約コメント追加 完了。ビルド警告ゼロ・151/151 PASS 確認済み。

---

## 1. 開発環境

| 項目 | 値 |
|------|-----|
| Target OS | Windows 11 |
| IDE/Compiler | Visual Studio 2022 (MSVC) |
| CMake Generator | `Visual Studio 17 2022` |
| Architecture | x64 / Release |
| C++ 標準 | C++17 |
| ビルドコマンド | `cmake --build build --config Release` |

---

## 2. 現在のステータス

### 現在の状態

| 検証項目 | 結果 |
|---------|------|
| ビルド (Service / Manager / Tests) | ✅ 警告ゼロ Release (2026-03-30) |
| ユニットテスト | ✅ 151/151 PASS (2026-03-30) |
| v1.1.0 リリース (§9.00〜§9.09 メモリ安定化) | ✅ 完了 (2026-03-26) |
| RegistryPolicyManager v5 (IFEO/PT split) | ✅ 実装完了 (2026-03-29) |
| CPU 暴走対策 (SetEvent 責務分離 + スピン検知) | ✅ 実装完了 (2026-03-29) |
| プロアクティブポリシー生成 (§9.12) | ✅ 実装完了 (2026-03-30) |
| CanonicalizePath 正規化統一 + 長パス対応 (§9.13) | ✅ 実装完了 (2026-03-30) |
| ログローテーション完全安定化 (§8.59 + §8.60) | ✅ 完了 (2026-03-24) |
| ログシーケンス逆転バグ修正 (§8.61) | ✅ 完了 (2026-03-25) |
| リリース前ソースコード整理 (§8.56) | ✅ 完了 (2026-03-16) |
| Service ETW 改善 | ✅ 保持 (§8.50 変更なし) |
| ウィンドウ位置保存 | ✅ 保持 (§8.52 変更なし) |

> **WDAC 注記 (解消)**: §8.42 で ctest による全 104 件の PASS を確認。WDAC によるブロックは現環境では発生せず。

### ドキュメント状態 (2026-03-30 更新)

| ファイル | 状態 |
|---------|------|
| `docs/Engine_Specification.md` | ⚠️ 要更新 (v1.0.3 のまま — RegistryPolicyManager v5 未反映) |
| `docs/Manager_Specification.md` | ✅ 最新 (v1.0.3・最終更新 2026-03-24) |
| `docs/UI_RULES.md` | ✅ 最新 (v1.0.3・最終更新 2026-03-24) |
| `docs/GitHub_CI_Operation.md` | ✅ 最新 (v1.0.3・最終更新 2026-03-24) |
| `README.md` | ⚠️ 要更新 (v1.0.3 のまま) |
| `README_EN.md` | ⚠️ 要更新 (v1.0.3 のまま) |
| `CHANGELOG.md` | ⚠️ 要更新 (v1.0.3 のまま — v5 改修未記載) |

---

## 3. アーキテクチャ概要 (安定・変更不要)

### Service (UnLeaf_Service.exe)

イベント駆動 3 スレッド構成:

| スレッド | ブロッキング方式 |
|---------|----------------|
| EngineControlLoop | `WaitForMultipleObjects(5 handles, INFINITE)` |
| ETW ConsumerThread | `ProcessTrace()` |
| IPC ServerThread | `ConnectNamedPipe()` |

WFMO 待機ハンドル: `stopEvent_` / `configChangeHandle_` / `safetyNetTimer_` / `enforcementRequestEvent_` / `hWakeupEvent_`

3フェーズ適応制御: AGGRESSIVE (即時 + 遅延検証 3 回) → STABLE (イベント駆動のみ) → PERSISTENT (5s SET + ETW ブースト)

### Manager (UnLeaf_Manager.exe)

2 スレッド構成:

| スレッド | ブロッキング方式 |
|---------|----------------|
| UI スレッド (メイン) | `GetMessage()` |
| logThread_ | `WaitForSingleObject(logWakeEvent_, 50ms)` |

IPC レート制御: WM_TIMER (1s) → 3 秒ゲート → `PostMessage(WM_IPC_REFRESH)`

---

## 4. ソースファイル構成

```
src/
├── common/
│   ├── types.h              VERSION 定数, ServiceState enum, CriticalSection, CanonicalizePath, NormalizePath
│   ├── logger.h/cpp         LightweightLogger (100KB ローテーション)
│   ├── config.h/cpp         UnLeafConfig (INI パーサ, FindFirstChangeNotification)
│   ├── registry_manager.h/cpp RegistryPolicyManager v5 (IFEO/PT split, per-entry state machine, lock-free pending queue, proactive reconciliation)
│   ├── scoped_handle.h      ScopedHandle / ScopedSCMHandle / WaitHandle RAII
│   └── win_string_utils.h/cpp unleaf::Utf8ToWide / WideToUtf8
├── engine/
│   ├── engine_policy.h      EnginePolicy 構造体 (タイミング定数集約、デフォルト値付き)
│   ├── engine_logic.h       純粋 C++ 決定ロジック (Win32 依存なし)
│   └── engine_logic.cpp     IsTargetProcess / IsCacheValid / ShouldExitPersistent 等
├── service/
│   ├── engine_core.h/cpp    EngineCore (WFMO ループ, 3フェーズ制御, SetEvent責務分離, スピン検知, CanonicalizePath, プロアクティブポリシー)
│   ├── process_monitor.h/cpp ETW セッション管理
│   ├── ipc_server.h/cpp     Named Pipe サーバー (DACL: SYSTEM + Admins)
│   └── service_main.cpp     SCM エントリーポイント
└── manager/
    ├── main_window.h/cpp    Win32 GUI (ダークテーマ固定, GDI+)
    ├── ipc_client.h/cpp     Named Pipe クライアント (lock 縮小設計)
    ├── service_controller.h/cpp SCM 操作
    └── main.cpp             WinMain (管理者権限昇格, GDI+ 初期化)
```

---

## 5. 主要定数 (engine_core.h)

| 定数 | 値 | 説明 |
|------|-----|------|
| `SAFETY_NET_INTERVAL` | 10,000 ms | Safety Net タイマー |
| `PERSISTENT_ENFORCE_INTERVAL` | 5,000 ms | PERSISTENT エンフォース間隔 |
| `PERSISTENT_CLEAN_THRESHOLD` | 60,000 ms | PERSISTENT → STABLE 復帰条件 |
| `LIVENESS_CHECK_INTERVAL` | 60,000 ms | ゾンビプロセス検出間隔 |
| `SUPPRESSION_CLEANUP_INTERVAL` | 60,000 ms | errorLogSuppression_ TTL クリーンアップ |
| `SUPPRESSION_TTL` | 300,000 ms | エラーログ抑制エントリ生存時間 |
| `SUPPRESSION_MAX_SIZE` | 2,000 | errorLogSuppression_ 緊急キャップ |
| `POLICY_CACHE_MAX_SIZE` | 1,024 | policyCacheMap_ LRU キャッシュ上限 |
| `POLICY_VERIFY_INTERVAL_MS` | 1,800,000 ms | VerifyAndRepair 実行間隔 (30分) |
| `MEM_LOG_INTERVAL_SHORT` | 10,000 ms | [MEM] ログ間隔 (起動後 30 分以内) |
| `MEM_LOG_INTERVAL_LONG` | 60,000 ms | [MEM] ログ間隔 (起動後 30 分以降) |
| `MEM_LOG_WARMUP_MS` | 1,800,000 ms | [MEM] ウォームアップ期間 (30 分) |
| `DIAG_LOG_INTERVAL_MS` | 30,000 (Debug) / 120,000 (Release) ms | [DIAG] ダンプ間隔 |
| `ETW_STABLE_RATE_LIMIT` | 200 ms | STABLE ETW レートリミット |
| `ECOQOS_CACHE_DURATION` | 100 ms | EcoQoS キャッシュ TTL |

---

## 6. ビルド出力

```
build\Release\
├── UnLeaf_Service.exe    (~748 KB)
├── UnLeaf_Manager.exe    (~749 KB)
├── UnLeaf_Tests.exe      (~957 KB, UNLEAF_BUILD_TESTS=ON 時のみ)
├── UnLeaf.ini            (自動生成)
└── UnLeaf.log            (100KB ローテーション)
```

ビルド:
```cmd
cmake --build build --config Release
```

テスト実行:
```cmd
ctest --test-dir build -C Release --output-on-failure
```

> **注意**: `UnLeaf_Manager.exe` 実行中は Manager の LNK1104 が出るが、Service と Tests のビルドは問題なし。

---

## 7. 完了済み主要実装 (変更履歴サマリ)

| セクション | 内容 | 完了日 |
|-----------|------|--------|
| §8〜8.2 | イベント駆動アーキテクチャ移行 + UAF 防止 + pending removal queue | 2026/02/13 |
| §8.3 | RC 改修 (Stop() ステップログ, タイマ戻り値検証, 起床カウンタ, エラーログ抑制) | 2026/02/14 |
| §8.4 | GoogleTest 導入 (71 テスト) | 2026/02/14 |
| §8.5〜8.7 | テレメトリ拡張, ゾンビプロセス除去, CPUバースト解消 | 2026/02/20 |
| §8.8〜8.9 | nlohmann/json 導入, RichEdit 差分追記, ログ色分け | 2026/02/20 |
| §8.10〜8.21 | Manager UI 全面改修 (DPI, GDI+, ダークテーマ固定, レイアウト整列) | 2026/02/21〜02/23 |
| §8.22 | Service Working Set 修正 (TTL クリーンアップ + コンテナ上限) | 2026/02/27 |
| §8.23 | IPC 最適化 (3秒ゲート) + [MEM] メモリ診断ログ | 2026/03/01 |
| §8.24〜8.25 | win_string_utils 追加, Utf8ToWide バグ修正, v1.00 最終ブラッシュアップ | 2026/03/02 |
| §8.26〜8.29 | ダブルバッファリング, goto cleanup, RGB 重複解消, UI_RULES.md 作成 | 2026/03/02 |
| §8.30 | [IPC]/[IPCQ] デッドコード完全削除 (Phase 2 テスト合格後) | 2026/03/03 |
| §8.31 | 製品版前最終レビュー 4 件修正 (hoveredButton_削除, SetNamedPipeHandleState チェック等) | 2026/03/03 |
| §8.32 | ドキュメント最新化 (Engine_Spec 6箇所, Manager_Spec 新規作成, README 開発者セクション) | 2026/03/03 |
| §8.33 | 設計耐久性検証 (memory_order 全網羅, Stop() タイムライン, ETW 競合, GDI 全監査) + 修正2件 | 2026/03/04 |
| §8.34 | v1.0.0 最終ドキュメント整合 (SemVer 統一, スレッドモデル明文化, 設計制約注記, 将来予定分離) | 2026/03/04 |
| §8.35 | Manager Phase A 長期計測合格確認 (42.74h / 5129 samples) | 2026/03/06 |
| §8.36 | Service Phase B 長期計測合格確認 (29.62h / 3554 samples) | 2026/03/08 |
| §8.37 | ToggleSubclassProc null チェック追加 (Phase A) | 2026/03/08 |
| §8.38 | Phase 0: DeleteTimerQueueTimer ロック外移動 (Fix A/B/C — 全5箇所、trackedCs_ 保持中の呼び出しを完全排除) | 2026/03/08 |
| §8.39 | DrawButton SelectObject 復元 (GDI ベストプラクティス対応、oldFont 保存/復元) | 2026/03/08 |
| §8.40 | Phase B-2: engine_logic 分離リファクタ (src/engine/ 新設、純粋 C++ 決定ロジック5関数抽出、ProcessPhase type alias 化) | 2026/03/08 |
| §8.41 | Phase B-3: engine_logic ユニットテスト追加 (tests/test_engine_logic.cpp 新規作成、32テストケース、102/102 PASS) | 2026/03/08 |
| §8.42 | Phase B-4: EnginePolicy 導入 (src/engine/engine_policy.h 新規作成、2関数シグネチャを EnginePolicy& に変更、engine_core.h に policy_ メンバ追加、呼び出し側4箇所更新、test_engine_policy.cpp 新規作成、104/104 PASS) | 2026/03/08 |
| §8.43 | Targeted Fix Patch: engine_logic.cpp default case コメント明確化 | 2026/03/08 |
| §8.44 | Engine_Specification.md ドキュメント同期 (§8.40〜§8.43 反映: engine_logic/engine_policy 節新設、§4.5/§13.5 追加) | 2026/03/08 |
| §8.45 | CI/CD 基盤追加 (GitHub Actions): `.github/workflows/build.yml` 新規作成。push/PR 時に windows-latest で configure → build → ctest を自動実行 | 2026/03/09 |
| §8.46 | v1.0.1 パッチ 2 件: ① GdipGuard RAII 構造体で GdiplusShutdown 確実呼び出し。② CI キャッシュパスを `build/_deps`、hashFiles グロブ化 | 2026/03/09 |
| §8.47 | v1.0.1 リリース前リポジトリ整備: `build/`・`Project_Cleanup_Prompt.md`・`UnLeaf_v1.0.0_Final.zip` 削除、`CHANGELOG.md` 新規作成、`logger.cpp` の `#include <cstdio>` 削除、`main_window.h` の `#include <deque>` 削除、`.gitignore` に `!CHANGELOG.md`・`!BUILD.md`・`out/`・`bin/`・`obj/`・`.vs/` 追加 | 2026/03/09 |
| §8.48 | Windows バージョン表示名修正 (`engine_core.cpp`): major=10, build>=22000 → `Windows 11`、major=10, build<22000 → `Windows 10`、major>=11 → `Windows {major}.{minor}` のハイブリッド方式。Windows 11 環境で `Windows 10.0` と誤表示されていたログを修正 | 2026/03/09 |
| §8.49 | ChatGPT マイクロ最適化提案レビュー (3件): ① `std::map→unordered_map` — 技術的に安全だが追跡対象は常時数件〜十数件のため効果なし、見送り。② `TrackedProcess` アロケーションチャーン — `make_shared` は `ApplyOptimization()` 内1箇所のみ・低頻度、チャーン不存在のため見送り。③ ETW ホットパス — コールバックはゼロアロケーション設計済み、見送り。**コード変更なし・現状維持が適切** | 2026/03/11 |
| §8.50 | ETW 信頼性改善 (v1.0.2 patch): ① Zombie Session Cleanup — `StartTraceW` 前に同名セッション停止 (ERROR_ALREADY_EXISTS 防止)。② Lost Event Detection — `EVENT_TRACE_TYPE_LOST_EVENT` opcode 検知・LOG_ALERT + 累積カウント (`lostEventCount_`)。③ Buffer Configuration 明示化 — `BufferSize=64KB`, `MinimumBuffers=4`, `MaximumBuffers=32`, `FlushTimer=0`。`VERSION` 定数を `L"1.0.2"` に更新 | 2026/03/12 |
| §8.51 | Manager ロギング統一 (`main_window.cpp` 4箇所): ① Logger 初期化 — `LightweightLogger::Instance().Initialize(baseDir_)` を Config 初期化直後に追加。② `AppendLog` 置換 — `LOG_INFO` によるファイル永続化・`PostMessageW(WM_LOG_REFRESH)` による UI 即時更新・`SetEvent(logWakeEvent_)` による watcher 起床・`IsEnabled()` 早期 return (UI/ファイル同時抑制) を実装。フォーマット `[ UI ]` → `[INFO] [Manager]` に統一。③ `IsManagerLine()` ヘルパー追加 — `] [Manager] ` パターンで `ProcessNewLogLines` の二重表示を排除。④ `GetLogLineColor` 更新 — `[ UI ]` → `[Manager]` チェック (シアン `RGB(80,220,220)`)、`[INFO]` より前に評価。Service 側・Logger・tests 変更なし | 2026/03/12 |
| §8.52 | ウィンドウ位置・状態永続化: ① `config.h` — `ManagerWindowState` struct 追加・`Get/SetManagerWindowState()` 宣言・`managerWindowState_` メンバ追加。② `config.cpp ParseIni` — `[Manager]` セクション追加・valid 判定強化 (`width>0 && height>0 && x>=-32768 && y>=-32768`)・ParseIni 先頭で `managerWindowState_` リセット。③ `config.cpp SerializeIni` — `[Manager]` セクション出力 (`valid` 時のみ・key 名 `WindowWidth`/`WindowHeight`)。④ `config.cpp` — `Get/Set` メソッド実装。⑤ `main_window.cpp Initialize()` — `ShowWindow` 前に保存済み位置を復元・`MonitorFromPoint(MONITOR_DEFAULTTONULL)` でオフスクリーン検出・`SWP_FRAMECHANGED` で Per-Monitor DPI ズレ防止。⑥ `main_window.cpp WM_DESTROY` — `GetWindowPlacement` + フィールド個別差分チェック + `Save()` (変化時のみ)。保存タイミングを WM_CLOSE → WM_DESTROY に変更 (WM_CLOSE キャンセル対策)。104/104 PASS 確認済み | 2026/03/12 |
| §8.53 | LoadInitialLogTail フィルター漏れ修正 + ShouldDisplayInUILog 一元化 | 2026/03/13 |
| §8.54 | ライブログ [DEBG] カラー修正 + RefreshLogDisplay endPos EM_GETTEXTLENGTHEX 修正 | 2026/03/13 |
| §8.55 | **v1.0.2 ライブログ切り戻し** (`main_window.cpp` のみ): 自動スクロール停止・空白行・二重表示の複合不具合を解消。`AppendLog` を `[ UI ]` タグ方式に戻す (LOG_INFO/SetEvent/PostMessageW/IsEnabled 削除)。`IsManagerLine`・`ShouldDisplayInUILog` ヘルパー削除。`GetLogLineColor` を `[ UI ]` タグ・RGB(100,100,115) に修正。`ProcessNewLogLines` を単純 `!line.empty()` チェックに戻す。`RefreshLogDisplay` から `EM_HIDESELECTION`・`PARAFORMAT2`・`oldSel` 保存/復元・`ScrollLogToBottom` を削除し、プラン確定版ループ (EM_EXGETSEL/EM_EXSETSEL) + `WM_VSCROLL SB_BOTTOM` に変更。`EM_GETTEXTLENGTHEX`・自動スクロール判定・`InvalidateRect` は保持 | 2026/03/14 |
| §8.56 | **リリース前ソースコード整理** (`registry_manager.cpp` のみ): `/src` 全 34 ファイル調査。① L8 未使用 `#include <algorithm>` 削除。② L127 stale コメント `// Registry Operations (migrated from registry_policy.cpp)` → `// Registry Operations` に簡略化。ビルド警告ゼロ・104/104 PASS 確認済み | 2026/03/16 |
| §8.57 | **Phase 3: LogEngine / LogQueue 基盤実装** (`src/manager/` 新設): `LogQueue` — スレッドセーフなログ行蓄積キュー + wake-up イベント。`LogEngine` — logThread_ ドライバ・ファイル Logger と UI の両配信を `UICallback` 登録機構で 1 ルート化。[LIVE-2] Manager 操作ログのファイル永続化を根本解消 | 2026/03/16 |
| §8.58 | **Phase 4: Virtual ListView ライブログ表示** (`src/manager/main_window.cpp` 他): RichEdit を廃止し、カスタム仮想 ListView + Owner-Draw レンダラーに置換。自動スクロール停止・空白行・二重表示の複合不具合を根本排除。[LIVE-3]/[LIVE-4] は RichEdit 廃止により不要化。ビルド警告ゼロ・104/104 PASS 確認済み | 2026/03/16 |
| §8.59 | **ログローテーション完全再設計** (`logger.h/cpp`, `service_main.cpp`, `ipc_server.cpp`): 7日間稼働テストで `UnLeaf.log.1` 未生成の重大不具合を確認・修正。① `FILE_SHARE_DELETE` 追加 (logger + ipc_server の全 CreateFileW) — MoveFileExW が ERROR_SHARING_VIOLATION で失敗する根本原因を解消。② `MoveFileW` → `MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)` に変更。③ Move 失敗時の `CREATE_ALWAYS` 禁止 — OPEN_ALWAYS でデータ保全、次 write サイクルでリトライ。④ ローテーション責務を Service のみに限定 — `SetRotationEnabled(true)` を service_main.cpp の 2 箇所に追加、Manager はデフォルト `false`。⑤ プロセス間排他 `Global\UnLeafLogRotation` Named Mutex 導入 (INFINITE 待機)。⑥ `RotationGuard` RAII — rotating_ フラグを例外安全に管理。⑦ `CheckStaleHandle()` — Manager が 100 write ごとに NTFS ファイル ID 比較でローテーション後の新 UnLeaf.log に自動収束。⑧ WriteFile ごとの FlushFileBuffers 廃止、rotation 直前のみに限定。⑨ Log() 呼び出しを mutex+RotationGuard スコープ外に移動し、rotating_ に依存しない再入安全設計を確立。104/104 PASS 確認済み | 2026/03/20 |
| §8.60 | **ログローテーション完全安定化** (`logger.h/cpp`): 第2回ローテーション時サービス停止の根本解決。① `CheckRotation()` を純粋な `RotationResult` 返却関数に変更 — `Log()` 呼び出しを構造的に禁止し、無限再帰 (スタックオーバーフロー) を根絶。② `SafeInternalLog()` 新設 — `WriteFile` 直接書き込みのみ、再帰不可能。③ `MoveFileExW` → `SetFileInformationByHandle(FileRenameInfoEx, POSIX_SEMANTICS)` に置換 — Manager が宛先ハンドルを保持していても原子的リネーム成功。④ `RotationMutexGuard` RAII 化 — `ReleaseMutex` 漏れを構造的に排除。⑤ `ScopedHandle` で rename ハンドルを管理 — リーク完全防止。⑥ `FlushFileBuffers` 失敗時にハンドルクローズ + INVALID 化 — 無限リトライスピン防止。⑦ `WriteFile` 失敗時に `enabled_ = false` — I/O エラーループ防止。⑧ `GetLastError()` を API 失敗直後にローカル変数へ即時取得。104/104 PASS 確認済み | 2026/03/24 |
| §9.10 | **RegistryPolicyManager v5 全面再設計** (`registry_manager.h/cpp`, `engine_core.h/cpp`, `types.h`, `test_types.cpp`): IFEO (exe 名単位) と PowerThrottle (パス単位) を分離し、同一 exe 名の複数パス (Chrome Stable + Canary 等) を独立管理可能に。① per-entry state machine (APPLYING → COMMITTED) で同一パスの並行 Apply を排他。② lock-free Treiber stack (atomic CAS) による pending removal queue。③ policyCs_ 保持中の I/O をゼロに（DC-2 準拠）。④ registry 書き込み後の IFEOKeyExists / PowerThrottleValueExists による実体検証 (REQ-5)。⑤ ifeoRefCount_ 参照カウントで IFEO キーの共有管理。⑥ LRU キャッシュ (list + unordered_map) で EngineCore 側の重複 Apply を排除。⑦ VerifyAndRepair (30 分間隔) で registry/memory 不整合を自動修復。⑧ SaveManifestAtomic (temp + FlushFileBuffers + MoveFileExW) で manifest クラッシュ安全性確保。⑨ IsCanonicalPathImpl + UNLEAF_ASSERT_CANONICAL マクロ追加。⑩ IsPolicyValid API による LRU キャッシュ実体乖離検出。151/151 PASS 確認済み | 2026/03/29 |
| §9.11 | **CPU 暴走対策 — SetEvent 責務分離 + スピン検知** (`engine_core.cpp`): 2 時間稼働で CPU 96.9% に達する重大バグを修正。根本原因: `ProcessPendingRemovals` 内の無条件 `SetEvent(hWakeupEvent_)` がキュー空でも EngineControlLoop を即座に再起床させる無限 wakeup ループ。① D1: ProcessPendingRemovals — `hasRemaining` フラグを lock 内で取得、SetEvent を lock 外で条件付き実行。② P2: Zombie detection — `wasEmpty` パターン統一。③ P3: Eviction — `wasEmpty` パターン統一。④ EngineControlLoop にスピン検知 (static thread_local `spinCount`、連続 10,000 回超で `[SPIN DETECTED]` ログ + 1ms sleep) を最終防衛線として追加。⑤ 設計コメント修正 (auto-reset event は signal 非保持 — 「次の drain がスケジュールされる」が正しい表現)。不変条件: push 側は empty→non-empty 遷移時のみ SetEvent、drain 側は残件ありの場合のみ SetEvent、wasEmpty 判定は `pendingRemovalCs_` ロック内。P1 (OnProcessStop) は既に正しい `wasEmpty` パターン実装済みで変更不要。151/151 PASS 確認済み | 2026/03/29 |
| §9.12 | **プロアクティブポリシー生成** (`engine_core.h/cpp`, `registry_manager.h/cpp`): リアクティブ（ETW 検出時のみ）からプロアクティブ（config 起点）へアーキテクチャ変更。① `ApplyProactivePolicies()` 新設 — Initialize/HandleConfigChange 時に全 config エントリへポリシー適用。② `ApplyIFEOOnly()` 新設 — name-only ターゲットの IFEO プロアクティブ適用。③ `ReconcileWithConfig()` 新設 — config から除外されたエントリの自動クリーンアップ（stateVersion_ CAS による race condition 防止）。④ `ResolvePathByHandle` 完全廃止 → `CanonicalizePath`（GetFullPathNameW ベース、ファイル存在不要）に置換。⑤ `HasPolicy()` 新設 + `ApplyOptimizationWithHandle` にフォールバック ApplyPolicy 復活（proactive 失敗時のリカバリ）。⑥ `FileExistsW()` 新設（GetFileAttributesW）— パス存在時は IFEO+PT、不在時は IFEO のみ適用。⑦ IFEO グローバルスコープの設計契約をコメント明記。151/151 PASS 確認済み | 2026/03/30 |
| §9.13 | **CanonicalizePath 正規化統一 + 長パス対応** (`types.h`, `engine_core.cpp`, `registry_manager.cpp`): ① `CanonicalizePath` を `types.h` に free function として配置（EngineCore・RegistryPolicyManager 双方から利用可能）。② GetFullPathNameW 2段階呼び出し（動的バッファ）で MAX_PATH 制限撤廃。③ `resultLen` による明示的文字列長指定（バッファ読み過ぎ防止）。④ policyMap_ の全キー生成・検索を `CanonicalizePath` に統一（`NormalizePath` はフォールバック/ログ用途に限定）。⑤ 設計契約コメント（DESIGN CONTRACT / DO NOT）を engine_core.cpp, registry_manager.cpp, registry_manager.h の3ファイルに追加。151/151 PASS 確認済み | 2026/03/30 |
| §8.61 | **ログシーケンス逆転バグ修正** (`logger.h/cpp`): ローテーション後 `UnLeaf.log` 先頭レコードの日時が `UnLeaf.log.1` 末尾より古くなる現象を修正。根本原因: Manager の stale check が `WriteFile` の**後**にあったため、最低 1 Write が旧ファイル (リネーム済み `.log.1`) に書かれていた。① `hRotationEvent_` (`Global\UnLeafLogRotated`, auto-reset named event) を追加 — Service はローテーション成功直後に `SetEvent`、Manager は `WriteFile` **前**に `WaitForSingleObject(..., 0)` でゼロ待機ポーリング。② stale check ブロックを `WriteFile` の後から前へ移動 — `rotationSignaled` で即時検知、counter フォールバック (100 write) も保持。③ stale reopen 失敗時の early return ガードを追加。これにより stale Write がゼロになりシーケンス逆転が解消。104/104 PASS 確認済み | 2026/03/25 |

---

## 8. リリース確認事項

### v1.0.3 リリース (2026-03-25) ✅

| 項目 | 状態 |
|------|------|
| `CheckRotation()` 完全再設計・無限再帰根絶 (§8.60) | ✅ 完了 |
| POSIX rename 導入 (§8.60) | ✅ 完了 |
| `RotationMutexGuard` RAII 化 (§8.60) | ✅ 完了 |
| `SafeInternalLog()` 新設 (§8.60) | ✅ 完了 |
| `FlushFileBuffers` / `WriteFile` エラーハンドリング (§8.60) | ✅ 完了 |
| ログシーケンス逆転バグ修正・`hRotationEvent_` 追加 (§8.61) | ✅ 完了 |
| `VERSION` 定数変更 → `L"1.0.3"` | ✅ 完了 |
| CHANGELOG.md 更新 | ✅ 完了 (2026-03-25) |
| README.md / README_EN.md 更新 | ✅ 完了 (2026-03-24) |
| docs/ ヘッダー 4 ファイル更新 | ✅ 完了 (2026-03-24) |
| ユニットテスト | ✅ 104/104 PASS (2026-03-25) |

### v1.0.2 リリース (2026-03-16) ✅

| 項目 | 状態 |
|------|------|
| ETW Zombie Session Cleanup (§8.50) | ✅ 完了 |
| ETW Lost Event Detection (§8.50) | ✅ 完了 |
| ETW Buffer Configuration 明示化 (§8.50) | ✅ 完了 |
| `VERSION` 定数変更 → `L"1.0.2"` | ✅ 完了 |
| Manager ロギング統一 (§8.51) | ✅ 完了 |
| ウィンドウ位置・状態永続化 (§8.52) | ✅ 完了 (104/104 PASS 確認済み) |
| LoadInitialLogTail フィルター漏れ修正 + ShouldDisplayInUILog 一元化 (§8.53) | ✅ 完了 |
| [DEBG] カラー修正 + RefreshLogDisplay endPos 修正 (§8.54) | ✅ 完了 |
| ライブログ切り戻し → Phase 3+4 で再実装 (§8.55→§8.57/58) | ✅ 完了 |
| ソースコード整理 (§8.56) | ✅ 完了 |
| LogEngine / LogQueue 基盤実装 (§8.57) | ✅ 完了 |
| Virtual ListView ライブログ表示 / RichEdit 廃止 (§8.58) | ✅ 完了 |
| CHANGELOG.md 更新 | ✅ 完了 (2026-03-16) |
| ドキュメント全体更新 | ✅ 完了 (2026-03-16) |

### v1.0.1 リリース (2026-03-09) ✅ (参考)

| 項目 | 状態 |
|------|------|
| バグ修正 4 件 (§8.37〜§8.39, §8.48) | ✅ 完了 |
| engine_logic 分離 + EnginePolicy 導入 (§8.40〜§8.43) | ✅ 完了 |
| ユニットテスト 104/104 PASS | ✅ 確認済み |
| GitHub Actions CI 導入 (§8.45) | ✅ 完了 |
| GdiplusShutdown RAII 化 + リポジトリ整備 (§8.46〜§8.47) | ✅ 完了 |

### v1.0.0 リリース (2026-03-06) ✅ (参考)

| 検証項目 | 結果 |
|---------|------|
| ビルド (Service / Manager / Tests) | ✅ 警告ゼロ |
| ユニットテスト | ✅ 104/104 PASS |
| Manager Phase A 長期計測 (42.74h) | ✅ 全指標合格 |
| Service Phase B 長期計測 (29.62h) | ✅ 全指標合格 |
| 設計耐久性検証 (§8.33) | ✅ 完了 (2026-03-04) |

---

## 9. v1.1.0 開発状況 + バックログ

### 9-0. v1.1.0 実装済み (未リリース)

| 項目 | 状態 |
|------|------|
| §9.00〜§9.09 メモリ安定化 | ✅ 完了 (2026-03-26) |
| §9.10 RegistryPolicyManager v5 (IFEO/PT split) | ✅ 実装完了 (2026-03-29) |
| §9.11 CPU 暴走対策 (SetEvent 責務分離 + スピン検知) | ✅ 実装完了 (2026-03-29) |
| §9.12 プロアクティブポリシー生成 | ✅ 実装完了 (2026-03-30) |
| §9.13 CanonicalizePath 正規化統一 + 長パス対応 | ✅ 実装完了 (2026-03-30) |
| ユニットテスト | ✅ 151/151 PASS (2026-03-30) |
| ドキュメント更新 (docs/, README, CHANGELOG) | ⚠️ 未実施 |
| `VERSION` 定数 | `L"1.1.0"` (変更済み) |

### 9-A. 解消済み項目

| 項目 | 解消状況 |
|------|---------|
| [LIVE-1] [DEBG] カラー精度改善 | ✅ §8.55 適用済み |
| [LIVE-2] Manager 操作ログのファイル永続化 | ✅ §8.57 UICallback 方式で解消 |
| [LIVE-3] RefreshLogDisplay — 選択状態保持 | ✅ RichEdit 廃止 (§8.58) により不要 |
| [LIVE-4] RefreshLogDisplay — PARAFORMAT2 行間制御 | ✅ RichEdit 廃止 (§8.58) により不要 |

---

### 9-B. v1.1+ バックログ

| 項目 | 優先度 | フェーズ | 備考 |
|-----|--------|---------|-----|
| WDAC ポリシー対応 | 中 | C | §8.42 で ctest 直接実行 PASS を確認。現環境では実害なし |
| ThemePalette 構造体への `clr*_` 集約 | 低 | C | §8.28 で RGB 重複解消済み。構造化は任意 |

フェーズ定義:
- Phase A: 安全性・保守性強化 (コード変更小・リスク低)
- Phase B: テスト可能性向上 (関数抽出 + テストコード追加)
- Phase C: インフラ・UX 改善 (長期・条件付き)

---

**Claude Code への指示**:
本ファイルは現在の作業状況を補足するための参考情報であり、設計判断は CLAUDE.md を最優先とする。
このファイルを読み込み、環境調査をスキップして作業を再開してください。
作業終了時には、必ずこのファイルを更新してください。
