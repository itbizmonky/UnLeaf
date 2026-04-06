# UnLeaf GitHub CI 運用手順書

Version: v1.1.0
最終更新: 2026-04-06

---

## CI Purpose

GitHub Actions CI は以下を保証する。

- **Automatic build verification**: push / pull_request のたびに Windows 環境でビルドを実行し、コンパイルエラーを即時検出する
- **Automatic unit test execution**: ctest により 151 件のユニットテストをすべて自動実行する
- **Prevention of broken commits**: ビルド失敗・テスト失敗のコミットが main ブランチに混入することを防ぐ

---

## 1. 目的

本手順書は、UnLeaf プロジェクトに導入された **GitHub Actions CI/CD 基盤**の運用方法を定義する。

CI により以下を自動化する。

* ソースコードの自動ビルド
* ユニットテスト実行（ctest）
* リグレッションの早期検出

---

## 2. CI 構成概要

CI workflow

```
.github/workflows/build.yml
```

処理フロー

```
Checkout
↓
CMake Configure
↓
Build
↓
CTest
```

実行環境

```
Runner : windows-latest
Build  : CMake
Test   : ctest
```

---

## 2.1 ビルドキャッシュ

CI は FetchContent の依存ライブラリ (GoogleTest / nlohmann_json) をキャッシュする。

| 項目 | 値 |
|------|-----|
| キャッシュパス | `build/_deps` |
| キャッシュキー | `${{ runner.os }}-cmake-${{ hashFiles('**/CMakeLists.txt') }}` |
| restore-keys | `${{ runner.os }}-cmake-` |

> `build/_deps` には FetchContent でダウンロードされる依存ライブラリのみが格納される。ビルド成果物 (.obj, .exe) は含まれないため、キャッシュサイズが小さく安定し、cache hit 率が高い。`hashFiles('**/CMakeLists.txt')` により、サブプロジェクトの CMakeLists.txt 変更も検知できる。

---

## 3. CI 実行トリガー

CI は以下で実行される。

| イベント         | CI |
| ------------ | -- |
| push         | 実行 |
| pull_request | 実行 |

---

## 4. 標準開発フロー

```
コード修正
↓
ローカル build
↓
ローカル ctest
↓
commit
↓
push
↓
GitHub Actions 実行
↓
成功 → 完了
失敗 → 修正
```

---

## 5. 開発手順

### ブランチ作成

```
git checkout -b feature/xxx
```

### ローカルビルド

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### ローカルテスト

```
ctest --test-dir build --output-on-failure
```

### commit

```
git add .
git commit -m "message"
```

### push

```
git push origin feature/xxx
```

---

## 6. CI確認

GitHub

```
Repository
↓
Actions
↓
build
```

---

## 7. CI成功条件

```
Build succeeded
All tests passed
```

---

## 8. CI失敗時

```
Actions
↓
build
↓
Run tests
```

失敗ログを確認して修正する。

---

## 9. 運用ルール

Rule1
push 前にローカルテスト実行

Rule2
CI failure を放置しない

Rule3
main ブランチは常に buildable に保つ

Rule4
大きな変更は Pull Request を使用

---

## 10. CI構成ファイル

```
.github/workflows/build.yml
```

---

## 11. 将来拡張

* clang-tidy
* coverage
* matrix build

---

## 12. 運用メリット

```
自動ビルド
自動テスト
品質保証
リグレッション検出
```
