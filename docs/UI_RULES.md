# UnLeaf Manager UI 設計規約

対象バージョン: v1.00
最終更新日: 2026-03-02
対象読者: UI 実装・保守を行う開発者

---

## 目次

1. [設計原則](#1-設計原則)
2. [描画規約](#2-描画規約)
3. [色管理規約](#3-色管理規約)
4. [レイアウト規約](#4-レイアウト規約)
5. [GDI リソース管理規約](#5-gdi-リソース管理規約)

---

## 1. 設計原則

UnLeaf Manager の UI 実装は以下の原則に従う。

| 原則 | 内容 |
|------|------|
| ダブルバッファ描画 | すべての描画はメモリ DC 経由で行い、`BitBlt` で転送する。直接描画は禁止 |
| OS 背景描画禁止 | `WM_ERASEBKGND` での描画を禁止し、`OnPaint` が唯一の描画箇所である |
| レイアウト計算の一元化 | RECT 計算・座標計算は `ComputeLayoutMetrics()` と `RepositionControls()` のみで行う |
| ロールベース色管理 | 色の決定は `ResolveRoleColor()` を唯一の経路とし、直接指定を禁止する |

---

## 2. 描画規約

### Rule 2-1: WM_ERASEBKGND は return TRUE のみ

```
Rule:     WM_ERASEBKGND ハンドラでは描画を行わず、return TRUE のみを返す。
Reason:   OS のデフォルト erase を無効化し、フリッカーの発生源を排除する。
Enforcement: WM_ERASEBKGND 内に FillRect・TextOut・SetPixel 等の描画呼び出しを書かない。
```

```cpp
case WM_ERASEBKGND:
    return TRUE;  // OS erase disabled; all painting in WM_PAINT
```

---

### Rule 2-2: 全描画は OnPaint のメモリ DC 経由

```
Rule:     描画はすべて OnPaint() 内のメモリ DC (hdcMem) に対して行い、BitBlt で転送する。
Reason:   ダブルバッファリングにより画面のフリッカーをゼロにする。
Enforcement: OnPaint 以外の場所で GetDC を取得して描画することを禁止する。
             ComputeLayoutMetrics() 内の GetDC はレイアウト計測専用であり描画禁止。
```

---

### Rule 2-3: 背景塗りは FillRect(hdcMem, …) のみ

```
Rule:     背景の塗りつぶしは OnPaint 内の FillRect(hdcMem, &rc, hBrushBg_) 一箇所で行う。
Reason:   塗りつぶし箇所が複数あると背景色の管理が分散し、テーマ変更時に不整合が生じる。
Enforcement: WM_ERASEBKGND および子コントロールのコールバック内で FillRect を使わない。
```

---

### Rule 2-4: OS テーマ色使用禁止

```
Rule:     GetSysColor() および COLOR_* 定数の使用を禁止する。
Reason:   UnLeaf はダーク固定アプリであり、OS テーマに追従しない。
Enforcement: clr*_ メンバ変数から色を取得する。GetSysColor() の呼び出しをコードに含めない。
```

---

## 3. 色管理規約

### Rule 3-1: RGB リテラル直書き禁止

```
Rule:     コード中に RGB() リテラルを直接記述することを禁止する。
          例外: MainWindow コンストラクタの色初期化ブロック（1箇所のみ）。
Reason:   リテラルが散在するとテーマ変更時の修正漏れが生じる。
Enforcement: RGB() の使用は初期化ブロック以外に存在しないこと。
```

---

### Rule 3-2: 色は clr*_ メンバ変数で管理する

```
Rule:     すべての色値は main_window.h の clr*_ メンバ変数として宣言し、初期化ブロックで設定する。
Reason:   色の定義箇所を一元化し、変更コストを最小にする。
Enforcement: 新しい色が必要な場合、clr*_ メンバを追加してから使用する。
             関数ローカルの COLORREF 変数に RGB() を直接代入しない。
```

現在管理されている色メンバ（`main_window.h`）:

| メンバ | 用途 |
|--------|------|
| `clrBackground_` | ウィンドウ背景 |
| `clrPanel_` | パネル背景 (リストボックス等) |
| `clrText_` | 標準テキスト |
| `clrTextDim_` | 非アクティブ・補助テキスト |
| `clrAccent_` | バージョン等アクセント表示 |
| `clrLogText_` | ログエリアテキスト |
| `clrOnline_` | サービス稼働状態 |
| `clrOffline_` | サービス停止状態 |
| `clrPending_` | 遷移中状態 (Gold) |
| `clrNotInstalled_` | サービス未インストール状態 |
| `clrButton_` / `clrButtonHover_` / `clrButtonBorder_` | ボタン状態 |

---

### Rule 3-3: 色解決は ResolveRoleColor() を唯一の経路とする

```
Rule:     コントロールの表示色は ResolveRoleColor(GetControlRole(hwndCtrl)) を通じて取得する。
Reason:   状態（ServiceState）と表示色の対応を一箇所で管理し、追加状態への対応を容易にする。
Enforcement: WM_CTLCOLORSTATIC ハンドラ内で直接 clr*_ を参照しない。
             新しい状態を追加する場合は ControlRole および ResolveRoleColor() を更新する。
```

---

### Rule 3-4: WM_CTLCOLOR* は所定のブラシを返す

```
Rule:     各 WM_CTLCOLOR* ハンドラが返すブラシは以下に固定する。
Reason:   コントロール背景色の統一管理。
Enforcement: ハンドラ内でブラシを動的生成しない。
```

| ハンドラ | 返すブラシ |
|---------|-----------|
| `WM_CTLCOLORSTATIC` | `hBrushBg_` |
| `WM_CTLCOLOREDIT` | `hBrushLogBg_` |
| `WM_CTLCOLORLISTBOX` | `hBrushPanel_` |

---

## 4. レイアウト規約

### Rule 4-1: レイアウト計算の唯一入口

```
Rule:     RECT 計算・コントロール配置はすべて ComputeLayoutMetrics() と
          RepositionControls() の中で完結させる。
Reason:   レイアウト責務を一元化し、DPI 変更・ウィンドウリサイズ時の
          計算漏れを構造的に防ぐ。将来の LayoutManager 昇格を容易にする。
Enforcement: 上記2関数以外に GetClientRect() を呼び出してコントロール座標を計算しない。
             WM_SIZE・OnPaint・UpdateServiceStatus 等から座標計算を行わない。
```

役割分担:

| 関数 | 責務 |
|------|------|
| `ComputeLayoutMetrics()` | DPI・フォントメトリクスから基本寸法を計算し、メンバ変数に保存する |
| `RepositionControls()` | メンバ変数を読んで MoveWindow() でコントロールを配置する |

---

### Rule 4-2: ハードコード座標禁止

```
Rule:     コントロールの座標・サイズに数値リテラルを直接記述することを禁止する。
Reason:   DPI・フォントサイズが変わると座標が崩れる。
Enforcement: すべてのサイズは DpiScale(value) または fontHeight_ 等のメンバ変数から導出する。
             MARGIN・INNER_PADDING はクラス定数として定義済みのものを使う。
```

---

### Rule 4-3: DPI スケーリングは DpiScale() を経由する

```
Rule:     ピクセル値を持つすべての定数・計算値は DpiScale(value) を経由する。
Reason:   高 DPI 環境で UI 要素が縮小・重複するのを防ぐ。
Enforcement: 定数への乗算ではなく DpiScale() を使う。フォント由来の値は tm.tmHeight 等を使う。
```

---

### Rule 4-4: 幅・高さは差分計算で求める

```
Rule:     ビットマップ・描画領域のサイズは rc.right - rc.left / rc.bottom - rc.top で求める。
Reason:   RECT の right/bottom は絶対座標であり、そのままサイズとして使うと不正なサイズになる。
Enforcement: CreateCompatibleBitmap(hdc, rc.right, rc.bottom) のような呼び出しを禁止する。
```

```cpp
// 正: 差分計算
int width  = rc.right  - rc.left;
int height = rc.bottom - rc.top;
hbmMem = CreateCompatibleBitmap(hdc, width, height);
```

---

## 5. GDI リソース管理規約

### Rule 5-1: GDI オブジェクトは cleanup ラベル方式で解放する

```
Rule:     OnPaint 等で複数の GDI オブジェクトを生成する場合、goto cleanup パターンを使い
          単一出口から解放する。
Reason:   早期 return が増えると解放漏れが生じやすい。一箇所の cleanup でリークを防ぐ。
Enforcement: GDI 生成後の早期 return を書かない。失敗時は goto cleanup に飛ばす。
             変数はすべて最初の goto より前に宣言し、jump-over-init を回避する。
```

---

### Rule 5-2: SelectObject で変更したオブジェクトは必ず復元する

```
Rule:     SelectObject() で DC に設定したフォント・ビットマップは、
          DeleteDC() の前に元のオブジェクトを SelectObject() で復元する。
Reason:   DC が保持するオブジェクトを未復元のまま DeleteDC すると GDI リークが発生する。
Enforcement: oldFont / hbmOld を変数に保存し、cleanup ブロックで復元する。
             null チェックを行ってから復元する。
```

```cpp
// cleanup ブロックの正しい順序
if (hdcMem && oldFont) SelectObject(hdcMem, oldFont);  // フォント復元
if (hdcMem && hbmOld)  SelectObject(hdcMem, hbmOld);   // ビットマップ復元
if (hbmMem)            DeleteObject(hbmMem);
if (hdcMem)            DeleteDC(hdcMem);
EndPaint(hwnd_, &ps);                                   // 常に呼ぶ
```

---

### Rule 5-3: EndPaint は常に呼ぶ

```
Rule:     OnPaint 内で BeginPaint を呼んだ場合、失敗パスを含むすべての経路で
          EndPaint を呼ぶ。
Reason:   EndPaint を呼ばないと WM_PAINT が無限に発行される。
Enforcement: cleanup ラベルの末尾に EndPaint を置き、goto cleanup で必ず到達させる。
             BeginPaint と EndPaint を別々の条件分岐に入れない。
```

---

### Rule 5-4: BitBlt 失敗はデバッグ出力で検知する

```
Rule:     BitBlt の戻り値を検査し、失敗時は OutputDebugStringW でログを残す。
Reason:   BitBlt 失敗は画面が真っ黒になる原因だが、例外が発生しないため検知が困難。
Enforcement: BitBlt の結果を無視しない。
```

```cpp
if (!BitBlt(hdc, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY)) {
    OutputDebugStringW(L"OnPaint BitBlt failed\n");
}
```

---

*本規約に違反する変更は、コードレビューで差し戻す。*
