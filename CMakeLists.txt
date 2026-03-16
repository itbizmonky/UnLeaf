# =============================================================================
# UnLeaf v1.00 - CMake Build Configuration
# オープンコア・モデル対応ビルド設定
#
# ビルド種別:
#   Internal build : src/manager が存在する場合 → Service + Manager をビルド
#   OSS build      : src/manager が存在しない場合 → Service のみをビルド
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(UnLeaf VERSION 1.0.0 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# C++ 標準設定
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# Windows 共通マクロ定義
# -----------------------------------------------------------------------------
if(WIN32)
    # Unicode 文字列 API を使用する (TCHAR → wchar_t)
    add_definitions(-DUNICODE -D_UNICODE)

    # Windows 10 以降を対象とする
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

    # 不要なヘッダの展開を抑制し、マクロ競合を防ぐ
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

# -----------------------------------------------------------------------------
# MSVC コンパイラフラグ
# -----------------------------------------------------------------------------
if(MSVC)
    # 警告レベル 4 (厳格なコンパイル品質ゲート)
    add_compile_options(/W4)

    # 意図的な未使用引数・変数に対する警告を抑制
    add_compile_options(/wd4100)  # unreferenced formal parameter
    add_compile_options(/wd4189)  # local variable initialized but not referenced

    # ソースファイルおよび実行文字セットを UTF-8 に統一
    add_compile_options(/utf-8)

    # 複数コアを使った並列コンパイル
    add_compile_options(/MP)

    # 厳格な標準準拠モード
    add_compile_options(/permissive-)

    # Release ビルドの最適化設定
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")

    # スタンドアロン実行ファイル用のスタティックランタイムリンク
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# =============================================================================
# 外部依存ライブラリ (FetchContent)
# =============================================================================
include(FetchContent)

# nlohmann/json: JSON パース・生成ライブラリ (IPC レスポンス等で使用)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# =============================================================================
# 共通ソースファイル (Service / Manager / Tests で共有)
# =============================================================================
set(COMMON_SOURCES
    src/common/logger.cpp           # ログ出力・ローテーション
    src/common/config.cpp           # INI 設定ファイル読み書き
    src/common/registry_manager.cpp # レジストリポリシー管理
    src/common/win_string_utils.cpp # UTF-8 ↔ ワイド文字列変換
)

set(COMMON_HEADERS
    src/common/types.h              # 共通型定義・定数
    src/common/scoped_handle.h      # RAII ハンドル管理
    src/common/logger.h
    src/common/config.h
    src/common/registry_manager.h
)

# =============================================================================
# UnLeaf_Service (エンジン) - 常時ビルド
# OSS ユーザー・作者ローカルの両方でビルドされます
# =============================================================================
message(STATUS "Building UnLeaf Service (Engine)...")

set(SERVICE_SOURCES
    src/service/main.cpp
    src/service/service_main.cpp
    src/service/engine_core.cpp
    src/engine/engine_logic.cpp
    src/service/process_monitor.cpp
    src/service/ipc_server.cpp
)

set(SERVICE_HEADERS
    src/service/service_main.h
    src/service/engine_core.h
    src/service/process_monitor.h
    src/service/ipc_server.h
)

# リソースファイル (アイコン・バージョン情報)
set(SERVICE_RESOURCES
    resources/service.rc
)

add_executable(UnLeaf_Service
    ${SERVICE_SOURCES}
    ${SERVICE_HEADERS}
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
    ${SERVICE_RESOURCES}
)

target_include_directories(UnLeaf_Service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(UnLeaf_Service PRIVATE
    advapi32             # Service Control Manager, ETW トレース制御
    kernel32             # Windows コア API
    user32               # ユーザーインターフェース
    tdh                  # Trace Data Helper (ETW イベントパース)
    nlohmann_json::nlohmann_json
)

# サービスはコンソールサブシステム (サービスディスパッチに必要)
set_target_properties(UnLeaf_Service PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "UnLeaf_Service"
)

# =============================================================================
# UnLeaf_Manager (UI) - 条件付きビルド
#
# src/manager ディレクトリが存在する場合のみビルドします。
# GitHub からクローンした OSS ユーザーの環境には src/manager が存在しないため、
# このブロックはスキップされ、エラーなく UnLeaf_Service だけがビルドされます。
# 作者のローカル環境では src/manager が存在するため、Manager もビルドされます。
# =============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/manager")
    message(STATUS "Internal build detected: Building UnLeaf Manager UI...")

    set(MANAGER_SOURCES
        src/manager/main.cpp
        src/manager/main_window.cpp
        src/manager/service_controller.cpp
        src/manager/ipc_client.cpp
        src/manager/log_engine.cpp
    )

    set(MANAGER_HEADERS
        src/manager/main_window.h
        src/manager/service_controller.h
        src/manager/ipc_client.h
        src/manager/resource.h
    )

    # リソースファイル (管理者権限マニフェストを含む)
    set(MANAGER_RESOURCES
        resources/app.rc
    )

    # WIN32 キーワードで Win32 GUI サブシステムを指定
    add_executable(UnLeaf_Manager WIN32
        ${MANAGER_SOURCES}
        ${MANAGER_HEADERS}
        ${COMMON_SOURCES}
        ${COMMON_HEADERS}
        ${MANAGER_RESOURCES}
    )

    target_include_directories(UnLeaf_Manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(UnLeaf_Manager PRIVATE
        advapi32             # Service Control Manager
        kernel32             # Windows コア API
        user32               # ウィンドウ管理
        gdi32                # GDI 描画
        shell32              # シェル関数
        comctl32             # コモンコントロール
        ole32                # COM (GDI+ 初期化等)
        shcore               # DPI アウェアネス
        nlohmann_json::nlohmann_json
    )

    set_target_properties(UnLeaf_Manager PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "UnLeaf_Manager"
    )

    # カスタムマニフェスト (RC ファイル経由) を使用するため自動生成を無効化
    if(MSVC)
        set_property(TARGET UnLeaf_Manager APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO")
    endif()

else()
    message(STATUS "OSS build detected: Skipping Manager UI.")
endif()

# =============================================================================
# インストール設定
# =============================================================================
install(TARGETS UnLeaf_Service RUNTIME DESTINATION bin)
if(TARGET UnLeaf_Manager)
    install(TARGETS UnLeaf_Manager RUNTIME DESTINATION bin)
endif()

# =============================================================================
# ユニットテスト (GoogleTest) - オプション
# =============================================================================
option(UNLEAF_BUILD_TESTS "Build unit tests" ON)

if(UNLEAF_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    # MSVC のスタティックランタイムと一致させる
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(UnLeaf_Tests
        tests/test_types.cpp
        tests/test_config.cpp
        tests/test_logger.cpp
        tests/test_engine_logic.cpp
        tests/test_engine_policy.cpp
        src/engine/engine_logic.cpp
        ${COMMON_SOURCES}
        ${COMMON_HEADERS}
    )

    target_include_directories(UnLeaf_Tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(UnLeaf_Tests PRIVATE
        GTest::gtest_main
        advapi32
        kernel32
        user32
    )

    include(GoogleTest)
    gtest_add_tests(TARGET UnLeaf_Tests)
endif()

# =============================================================================
# ビルド構成サマリ
# =============================================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "UnLeaf v${PROJECT_VERSION} Build Configuration")
message(STATUS "========================================")
message(STATUS "C++ Standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler     : ${CMAKE_CXX_COMPILER_ID}")
if(TARGET UnLeaf_Manager)
    message(STATUS "Targets      : UnLeaf_Service, UnLeaf_Manager (Internal)")
else()
    message(STATUS "Targets      : UnLeaf_Service (OSS)")
endif()
message(STATUS "Tests        : ${UNLEAF_BUILD_TESTS}")
message(STATUS "========================================")
message(STATUS "")
