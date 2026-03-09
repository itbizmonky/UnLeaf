# UnLeaf GitHub CI 運用手順書

Version: v1.00
最終更新: 2026-03-09

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
