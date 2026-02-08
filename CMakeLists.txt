# UnLeaf v6.00 - CMake Build Configuration
# Windows Native C++ Implementation with Multi-layer EcoQoS Defense

cmake_minimum_required(VERSION 3.20)
project(UnLeaf VERSION 6.0.0 LANGUAGES CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows-specific settings
if(WIN32)
    # Unicode support
    add_definitions(-DUNICODE -D_UNICODE)

    # Windows version targeting (Windows 10+)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

    # Lean Windows headers and prevent macro conflicts
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

# Compiler-specific flags
if(MSVC)
    # Warning level 3 (less strict to avoid excessive warnings)
    add_compile_options(/W3)

    # Disable specific warnings
    add_compile_options(/wd4100)  # unreferenced formal parameter
    add_compile_options(/wd4189)  # local variable initialized but not referenced

    # UTF-8 source and execution charset
    add_compile_options(/utf-8)

    # Enable multi-processor compilation
    add_compile_options(/MP)

    # Permissive mode off for better standards compliance
    add_compile_options(/permissive-)

    # Optimize for speed in release
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")

    # Static runtime linkage for standalone executables
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Common source files
set(COMMON_SOURCES
    src/common/logger.cpp
    src/common/config.cpp
)

set(COMMON_HEADERS
    src/common/types.h
    src/common/scoped_handle.h
    src/common/logger.h
    src/common/config.h
)

# ============================================================
# UnLeaf Service (Engine) - Windows Service Executable
# ============================================================
set(SERVICE_SOURCES
    src/service/main.cpp
    src/service/service_main.cpp
    src/service/engine_core.cpp
    src/service/process_monitor.cpp
    src/service/ipc_server.cpp
    src/service/registry_policy.cpp
)

set(SERVICE_HEADERS
    src/service/service_main.h
    src/service/engine_core.h
    src/service/process_monitor.h
    src/service/ipc_server.h
    src/service/registry_policy.h
)

add_executable(UnLeaf_Service
    ${SERVICE_SOURCES}
    ${SERVICE_HEADERS}
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
)

target_include_directories(UnLeaf_Service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link Windows libraries
target_link_libraries(UnLeaf_Service PRIVATE
    advapi32   # Service Control Manager, ETW trace control
    kernel32   # Core Windows API
    user32     # User interface
    tdh        # Trace Data Helper (ETW event parsing)
)

# Service is a console application (required for service dispatch)
set_target_properties(UnLeaf_Service PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "UnLeaf_Service"
)

# ============================================================
# UnLeaf Manager (UI) - Win32 GUI Application
# ============================================================
set(MANAGER_SOURCES
    src/manager/main.cpp
    src/manager/main_window.cpp
    src/manager/service_controller.cpp
    src/manager/ipc_client.cpp
)

set(MANAGER_HEADERS
    src/manager/main_window.h
    src/manager/service_controller.h
    src/manager/ipc_client.h
    src/manager/resource.h
   #  src/manager/UnLeaf.rc
)

# Resource file (includes manifest for admin privileges)
set(MANAGER_RESOURCES
    resources/app.rc
)

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

# Link Windows libraries
target_link_libraries(UnLeaf_Manager PRIVATE
    advapi32   # Service Control Manager
    kernel32   # Core Windows API
    user32     # Window management
    gdi32      # Graphics
    shell32    # Shell functions
    comctl32   # Common controls
    ole32      # COM
    shcore     # DPI awareness
)

set_target_properties(UnLeaf_Manager PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "UnLeaf_Manager"
)

# Disable auto-generated manifest, use our custom one via RC
if(MSVC)
    set_property(TARGET UnLeaf_Manager APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO")
endif()

# ============================================================
# Installation
# ============================================================
install(TARGETS UnLeaf_Service UnLeaf_Manager
    RUNTIME DESTINATION bin
)

# ============================================================
# Build Info
# ============================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "UnLeaf v${PROJECT_VERSION} Build Configuration")
message(STATUS "========================================")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "========================================")
message(STATUS "")
