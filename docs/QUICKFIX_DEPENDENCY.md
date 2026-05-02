# QuickFIX/C++ Dependency

## Installed Version

| Property | Value |
|----------|-------|
| **Package** | QuickFIX (Hardened Fork) |
| **Version** | 1.16.1-hardened |
| **Source** | https://github.com/dudleylane/quickfix |
| **Library** | `/usr/local/lib/libquickfix.so.17.0.0` (shared, 1.5 MB) |
| **Headers** | `/usr/local/include/quickfix/` (94 core headers) |
| **Data Dictionaries** | `/usr/local/share/quickfix/` (9 XML files) |
| **Build flags** | `-O3 -march=native -flto -DENABLE_TBB_ALLOCATOR -std=c++23` |

## File Locations

```
/usr/local/
├── include/quickfix/
│   ├── Application.h          # Core Application interface
│   ├── Acceptor.h             # Base acceptor (start/stop)
│   ├── ThreadedSocketAcceptor.h  # Thread-per-session acceptor
│   ├── SessionSettings.h      # INI config parser
│   ├── SessionID.h            # BeginString:Sender->Target
│   ├── Session.h              # sendToTarget() static method
│   ├── FileStore.h            # Persistent message store
│   ├── FileLog.h              # File-based logging
│   ├── FieldMap.h             # GroupArena bump-pointer allocator
│   ├── FixValues.h            # FIX enum constants (Side_BUY, OrdType_LIMIT, etc.)
│   ├── FixFields.h            # Typed field classes (ClOrdID, Price, Side, etc.)
│   ├── fix44/                 # FIX 4.4 message classes (95 headers)
│   │   ├── NewOrderSingle.h
│   │   ├── ExecutionReport.h
│   │   ├── OrderCancelRequest.h
│   │   ├── OrderCancelReplaceRequest.h
│   │   ├── OrderCancelReject.h
│   │   ├── MessageCracker.h   # Type-safe message dispatch
│   │   └── ...
│   └── ...
├── lib/
│   ├── libquickfix.so → libquickfix.so.17
│   ├── libquickfix.so.17 → libquickfix.so.17.0.0
│   └── libquickfix.so.17.0.0
└── share/quickfix/
    ├── FIX40.xml
    ├── FIX41.xml
    ├── FIX42.xml
    ├── FIX43.xml
    ├── FIX44.xml    ← used by OrderProcessor
    ├── FIX50.xml
    ├── FIX50SP1.xml
    ├── FIX50SP2.xml
    └── FIXT11.xml
```

## Runtime Dependencies

The shared library links to:

| Library | System Path | Purpose |
|---------|-------------|---------|
| `libssl.so.3` | `/lib64/libssl.so.3` | TLS/SSL for encrypted FIX sessions |
| `libcrypto.so.3` | `/lib64/libcrypto.so.3` | Cryptographic primitives |
| `libtbbmalloc.so.2` | `/lib64/libtbbmalloc.so.2` | TBB scalable allocator |
| `libstdc++.so.6` | `/lib64/libstdc++.so.6` | C++ standard library |

## CMake Integration

The OrderProcessor links QuickFIX conditionally via `BUILD_FIX`:

```cmake
# CMakeLists.txt
option(BUILD_FIX "Build with QuickFIX/C++ FIX gateway (requires quickfix)" OFF)
if(BUILD_FIX)
    find_library(QUICKFIX_LIB quickfix REQUIRED)
endif()

# Each target (server, test, bench) then links ${QUICKFIX_LIB}:
if(BUILD_FIX)
    target_link_libraries(<target> PRIVATE ${QUICKFIX_LIB})
    target_compile_definitions(<target> PRIVATE BUILD_FIX)
    target_include_directories(<target> PRIVATE /usr/local/include)
endif()
```

Build commands:

```bash
# Without FIX (default, no QuickFIX dependency)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# With FIX gateway enabled
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=ON
cmake --build build -j$(nproc)
```

## GCC 15 / C++23 Compatibility

The previous HFT fork's `Config23.h` injected `boost::container::flat_map` into `namespace std` as a polyfill. This hardened fork **does not include `Config23.h`** — the polyfill and its associated conflicts are eliminated.

No ordering constraints on QuickFIX includes vs `<flat_map>` includes. GCC 15's native `std::flat_map` is used everywhere.

## Key Differences from Previous HFT Fork

| Aspect | Previous (1.17.0-hft) | Current (hardened fork) |
|--------|----------------------|------------------------|
| `Config23.h` | Polyfills `std::flat_map` via Boost | Not present — no polyfill |
| `HFTOptimizations.h` | Branch hints, prefetch macros | Not present — not needed |
| `CompileTimeOptimizations.h` | Compile-time tag validation | Not present — not needed |
| `QUICKFIX_HFT_OPTIMIZED` | Defined | Not defined |
| `QUICKFIX_LOCK_FREE` | Defined | Not defined |
| `QUICKFIX_ZERO_COPY` | Defined | Not defined |
| PostgreSQL | Linked | Not linked (unused by OrderProcessor) |
| TBB allocator | Not available | Linked (`libtbbmalloc.so.2`) |
| GroupArena | Not available | Per-message bump-pointer arena for groups |
| Thread safety | Hand-rolled recursive mutex (data race) | `PTHREAD_MUTEX_RECURSIVE` (per-session), `std::shared_mutex` (global session registry) |
| `sizeof(Group)` | 120 bytes | 128 bytes (**ABI break — recompile required**) |

## ABI Break Notice

`sizeof(FIX::FieldMap)` changed from 112 → 120 and `sizeof(FIX::Group)` from 120 → 128 due to the addition of `std::unique_ptr<GroupArena> m_arena`. Any code compiled against the previous HFT fork headers **must be recompiled** against the new headers, or struct layout mismatches will cause crashes.

```bash
# After installing the new QuickFIX, clean and rebuild OrderProcessor:
cd /home/ojohnson/OrderProcessor
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=ON
cmake --build build -j$(nproc)
```

## FIX Protocol Version

The OrderProcessor uses **FIX 4.4** exclusively. The data dictionary is copied from the QuickFIX installation to the project:

```
/usr/local/share/quickfix/FIX44.xml → config/spec/FIX44.xml
```

The session configuration (`config/fix.cfg`) references this dictionary:

```ini
[DEFAULT]
BeginString=FIX.4.4
DataDictionary=config/spec/FIX44.xml
```

## Installation from Source

```bash
git clone https://github.com/dudleylane/quickfix.git
cd quickfix
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -flto" \
  -DCMAKE_EXE_LINKER_FLAGS="-flto" \
  -DCMAKE_SHARED_LINKER_FLAGS="-flto" \
  -DHAVE_SSL=ON \
  -DENABLE_TBB_ALLOCATOR=ON \
  -DQUICKFIX_SHARED_LIBS=ON
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig
```

Verify:

```bash
ls /usr/local/lib/libquickfix.so       # symlink exists
ldd /usr/local/lib/libquickfix.so.17   # shows libssl, libtbbmalloc
```
