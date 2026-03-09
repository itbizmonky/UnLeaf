# UnLeaf v1.0.1 - Build Instructions

## Prerequisites

- **Visual Studio 2019/2022** with C++ Desktop Development workload
- **CMake 3.20+**
- **Windows SDK 10.0.19041.0+**

## Build Steps

### Using Visual Studio

1. Open Visual Studio
2. Select "Open a local folder" or "File > Open > Folder"
3. Navigate to the `UnLeaf` folder
4. Visual Studio will auto-detect CMakeLists.txt
5. Select build configuration (Release recommended)
6. Build > Build All

### Using CMake CLI

```powershell
# Clone the repository
git clone https://github.com/itbizmonky/UnLeaf.git
cd UnLeaf

# Create build directory and configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release

# Output files will be in build/Release/
```

### Using Developer Command Prompt

```cmd
cd UnLeaf
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Run Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
# Expected: 104/104 tests passed
```

## Output Files

After successful build:
- `build/Release/UnLeaf_Service.exe` - Background service (~200KB)

> **Note**: The Manager UI (`UnLeaf_Manager.exe`) is closed-source and not included in this repository. The OSS `CMakeLists.txt` builds the service engine only.

## Deployment

1. Copy `UnLeaf_Service.exe` to the target directory
2. Copy `UnLeaf.ini` configuration file (or let it auto-create on first run)
3. Run `UnLeaf_Manager.exe` as Administrator to register the service

## Project Structure

```
UnLeaf/
├── .github/
│   └── workflows/
│       └── build.yml            # GitHub Actions CI (build + ctest on push/PR)
├── CHANGELOG.md                 # Release history
├── CMakeLists.txt               # OSS dynamic build script
├── LICENSE                      # MIT License
├── README.md / README_EN.md
├── docs/
│   └── Engine_Specification.md  # Detailed engine technical specification
├── resources/
│   └── service.rc               # Service resource file
├── src/
│   ├── common/                  # Shared utilities
│   │   ├── types.h              # Type definitions, constants
│   │   ├── scoped_handle.h      # RAII handle wrappers
│   │   ├── logger.h/cpp         # Rotating file logger
│   │   ├── config.h/cpp         # INI configuration manager
│   │   ├── registry_manager.h/cpp # Registry read/write helpers
│   │   ├── security.h           # DACL / ACL utilities
│   │   └── win_string_utils.h/cpp # UTF-8 / wide string conversion
│   ├── engine/                  # Engine decision logic (Win32-independent, pure C++)
│   │   ├── engine_logic.h/cpp   # Phase transitions & EcoQoS enforcement (5 functions)
│   │   └── engine_policy.h      # Timing constants (EnginePolicy struct)
│   ├── service/                 # Core engine (ETW monitoring, service control)
│   │   ├── main.cpp             # Entry point
│   │   ├── service_main.*       # Windows service framework
│   │   ├── engine_core.*        # Process monitoring / optimization
│   │   ├── process_monitor.*    # ETW-based process lifecycle tracking
│   │   └── ipc_server.*         # Named pipe server
│   └── manager/                 # Manager UI (closed-source, not built by OSS CMake)
└── tests/                       # Unit tests (104 cases / all PASS)
```

## CI/CD

GitHub Actions runs automatically on every `push` and `pull_request`:

1. **Build** — `cmake -B build` + `cmake --build build --config Release`
2. **Test** — `ctest --test-dir build -C Release --output-on-failure`

The workflow file is at `.github/workflows/build.yml`. FetchContent dependencies are cached at `build/_deps` for faster CI runs.

## Architecture Notes

### Event-Driven Design

UnLeaf uses ETW (Event Tracing for Windows) for process lifecycle detection — not periodic polling. This means:
- Zero CPU usage at idle (`WaitForMultipleObjects(INFINITE)`)
- Sub-millisecond reaction time when a target process spawns

### Key Components

| Component | Location | Description |
|-----------|----------|-------------|
| Engine logic | `src/engine/engine_logic.*` | Phase transitions, EcoQoS enforcement decisions (Win32-free, fully unit-testable) |
| Engine policy | `src/engine/engine_policy.h` | All timing constants in one place (`EnginePolicy` struct) |
| ETW monitoring | `src/service/engine_core.*` | Kernel event subscription, process tracking |
| Process monitor | `src/service/process_monitor.*` | ETW session management |
| IPC server | `src/service/ipc_server.*` | Named pipe communication with Manager UI |
| Registry manager | `src/common/registry_manager.*` | PowerThrottling + IFEO registry policy management |

### Benefits of Native C++

- **Zero runtime dependencies** — No Python interpreter needed
- **Minimal footprint** — ~700KB total vs ~50MB+ for Python
- **Lower resource usage** — Direct Win32/NT APIs
- **Better integration** — Native Windows service with ETW
