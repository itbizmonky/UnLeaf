# UnLeaf

**Windows 11 の「効率モード」(EcoQoS) を自動無効化するサービスユーティリティ**

Originally created by kbn.

---

## これは何？ 何を解決するのか

Windows 11 は「EcoQoS」という省電力機能を備えており、OS がバックグラウンドと判断したプロセスの CPU 周波数とスケジューリング優先度を自動的に引き下げます。タスクマネージャーで葉っぱのアイコン (効率モード) が表示されるのがこの機能です。

これは省電力には有効ですが、以下のような問題が発生します:

- **ブラウザ** (Chrome / Edge): バックグラウンドタブの処理が極端に遅くなる
- **ゲーム**: Alt-Tab 後にパフォーマンスが低下する
- **DAW / 動画編集**: レンダリング中にプロセスが throttle される

UnLeaf はこの問題を自動で解決します。指定したプロセスの EcoQoS を常に無効化し、OS が勝手にパフォーマンスを下げるのを防ぎます。

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
| OS | Windows 11 (Build 22000 以降) |
| 権限 | 管理者権限 |
| メモリ | 約 2MB |
| ディスク | 約 1.2MB |

> **注意**: Windows 10 では EcoQoS 機能自体が存在しないため、本ツールは不要です。

---

## インストール方法 (バイナリ ZIP)

1. GitHub Releases から最新の ZIP をダウンロード
2. 任意のフォルダに展開
3. `install_service.bat` を**右クリック → 管理者として実行**
4. `UnLeaf_Manager.exe` を起動してターゲットプロセスを設定

```
UnLeaf_v1.00/
├── UnLeaf_Service.exe      サービス本体
├── UnLeaf_Manager.exe      GUI 管理ツール
├── UnLeaf.ini              設定ファイル
├── install_service.bat     サービス登録
├── uninstall_service.bat   サービス削除
├── README.md
├── RELEASE_NOTES.md
└── LICENSE
```

---

## ビルド方法 (ソースから)

### 要件

- Visual Studio 2022 以降 (C++ Desktop Development ワークロード)
- CMake 3.20 以降
- Windows SDK 10.0.19041.0 以降

### 手順

```powershell
git clone https://github.com/<your-repo>/UnLeaf.git
cd UnLeaf

mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

ビルド成果物は `build/Release/` に出力されます。

---

## アンインストール

1. `uninstall_service.bat` を**右クリック → 管理者として実行**
2. UnLeaf フォルダを削除

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
Windows 11 で導入された省電力 API です。OS がプロセスの CPU 使用効率を下げることで電力消費を抑えます。タスクマネージャーの「効率モード」(葉っぱアイコン) として表示されます。

### CPU 使用率は上がりませんか？
**上がりません。** UnLeaf はイベント駆動で動作しており、監視対象のイベントがないときは `WaitForMultipleObjects(INFINITE)` で完全にスリープしています。アイドル時の CPU 使用率は 0% です。

### セキュリティリスクはありますか？
以下のセキュリティ対策を実装しています:
- **DACL**: IPC 通信は SYSTEM + Administrators のみアクセス可能
- **入力バリデーション**: プロセス名のパストラバーサル検査、長さ制限
- **保護リスト**: csrss.exe, lsass.exe, svchost.exe 等のシステム重要プロセスは操作対象外
- **権限分離**: コマンドごとに PUBLIC / ADMIN / SYSTEM_ONLY の権限レベル

### Windows 10 で使えますか？
Windows 10 には EcoQoS 機能自体が存在しないため、本ツールは不要です。

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
`UnLeaf_Manager.exe` を起動し、ヘルスチェックボタンでサービスの状態を確認できます。コマンドラインからは `sc query UnLeafService` でも確認可能です。

---

## ライセンス

[MIT License](LICENSE)
