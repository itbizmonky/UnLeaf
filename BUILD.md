# UnLeaf v1.00 - Build Instructions

## Prerequisites

- **Visual Studio 2019/2022** with C++ Desktop Development workload
- **CMake 3.20+**
- **Windows SDK 10.0.19041.0+**

## Build Steps

### Using Visual Studio

1. Open Visual Studio
2. Select "Open a local folder" or "File > Open > Folder"
3. Navigate to `UnLeaf_v1.00` folder
4. Visual Studio will auto-detect CMakeLists.txt
5. Select build configuration (Release recommended)
6. Build > Build All

### Using CMake CLI

```powershell
# Navigate to project directory
cd UnLeaf_v1.00

# Create build directory
mkdir build
cd build

# Configure (Release build)
cmake .. -G "Visual Studio 17 2022" -A x64

# Build
cmake --build . --config Release

# Output files will be in build/Release/
```

### Using Developer Command Prompt

```cmd
cd UnLeaf_v1.00
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Output Files

After successful build:
- `UnLeaf_Service.exe` - Background service (~200KB)
- `UnLeaf_Manager.exe` - GUI application (~500KB)

## Deployment

1. Copy both executables to target directory
2. Copy `UnLeaf.json` configuration file (or let it auto-create)
3. Run `UnLeaf_Manager.exe` as Administrator to register service

## Project Structure

```
UnLeaf_v1.00/
├── CMakeLists.txt          # Build configuration
├── BUILD.md                # This file
├── Design_Specification_v1.00.md
├── src/
│   ├── common/             # Shared utilities
│   │   ├── types.h         # Type definitions, constants
│   │   ├── scoped_handle.h # RAII handle wrappers
│   │   ├── logger.h/cpp    # Rotating file logger
│   │   └── config.h/cpp    # JSON configuration manager
│   ├── service/            # Background service
│   │   ├── main.cpp        # Entry point
│   │   ├── service_main.*  # Windows service framework
│   │   ├── engine_core.*   # Process monitoring/optimization
│   │   └── ipc_server.*    # Named pipe server
│   └── manager/            # GUI application
│       ├── main.cpp        # Entry point
│       ├── main_window.*   # Win32 GUI
│       ├── service_controller.* # SCM wrapper
│       └── ipc_client.*    # Named pipe client
├── include/                # External headers (nlohmann/json if needed)
└── resources/              # RC files, icons
```

## Architecture Notes

### Python v0.xx → C++ v1.00 Translation

| Feature | Python | C++ |
|---------|--------|-----|
| Process enumeration | `psutil.process_iter()` | `CreateToolhelp32Snapshot` |
| Process start detection | WMI `Win32_ProcessStartTrace` | Periodic snapshot diff |
| Process exit detection | WMI `Win32_ProcessStopTrace` | `RegisterWaitForSingleObject` |
| EcoQoS control | `ctypes` | Direct `SetProcessInformation` |
| Logging | `RotatingFileHandler` | Custom `LightweightLogger` |
| IPC | File-based | Named Pipes |
| GUI | CustomTkinter | Win32 API |

### Benefits of Native C++

- **Zero runtime dependencies** - No Python interpreter needed
- **Minimal footprint** - ~700KB total vs ~50MB+ for Python
- **Lower resource usage** - Native APIs are more efficient
- **Faster startup** - No interpreter initialization
- **Better integration** - Native Windows service support
