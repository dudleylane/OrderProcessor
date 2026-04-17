# QuickFIX/C++ Dependency

## Installed Version

| Property | Value |
|----------|-------|
| **Package** | QuickFIX-HFT (C++23 HFT fork) |
| **Version** | 1.17.0-hft |
| **Source** | https://github.com/dudleylane/claude-code-quickfix |
| **Library** | `/usr/local/lib/libquickfix.so.17.0.0` (shared, 1.6 MB) |
| **Headers** | `/usr/local/include/quickfix/` (97 core headers) |
| **Data Dictionaries** | `/usr/local/share/quickfix/` (9 XML files) |
| **pkg-config** | `/usr/local/lib/pkgconfig/quickfix-hft.pc` |
| **Build flags** | `-DQUICKFIX_HFT_OPTIMIZED=1 -DQUICKFIX_LOCK_FREE=1 -DQUICKFIX_ZERO_COPY=1 -std=c++23` |

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
│   ├── FixValues.h            # FIX enum constants (Side_BUY, OrdType_LIMIT, etc.)
│   ├── FixFields.h            # Typed field classes (ClOrdID, Price, Side, etc.)
│   ├── Config23.h             # C++23 feature detection (see compatibility note)
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
│   ├── libquickfix.so.17.0.0
│   └── pkgconfig/quickfix-hft.pc
└── share/quickfix/
    ├── FIX40.xml    (861 lines)
    ├── FIX41.xml    (1,281 lines)
    ├── FIX42.xml    (2,742 lines)
    ├── FIX43.xml    (4,229 lines)
    ├── FIX44.xml    (6,599 lines)   ← used by OrderProcessor
    ├── FIX50.xml    (8,141 lines)
    ├── FIX50SP1.xml (9,505 lines)
    ├── FIX50SP2.xml (26,068 lines)
    └── FIXT11.xml   (251 lines)
```

## Runtime Dependencies

The shared library links to:

| Library | System Path | Purpose |
|---------|-------------|---------|
| `libssl.so.3` | `/lib64/libssl.so.3` | TLS/SSL for encrypted FIX sessions |
| `libcrypto.so.3` | `/lib64/libcrypto.so.3` | Cryptographic primitives |
| `libpq.so.5` | `/usr/pgsql-18/lib/libpq.so.5` | PostgreSQL client (optional store backend) |
| `libstdc++.so.6` | `/lib64/libstdc++.so.6` | C++ standard library |
| `libz.so.1` | `/lib64/libz.so.1` | Compression |
| `libgssapi_krb5.so.2` | `/lib64/libgssapi_krb5.so.2` | Kerberos (via PostgreSQL) |

Minimum glibc: 2.38 (`GLIBC_2.38` symbol dependency).

## CMake Integration

The OrderProcessor links QuickFIX conditionally via `BUILD_FIX`:

```cmake
# CMakeLists.txt
option(BUILD_FIX "Build with QuickFIX/C++ FIX gateway (requires quickfix)" OFF)

if(BUILD_FIX)
    find_library(QUICKFIX_LIB quickfix REQUIRED)
    target_link_libraries(orderProcessorServer PRIVATE ${QUICKFIX_LIB})
    target_compile_definitions(orderProcessorServer PRIVATE BUILD_FIX)
    target_include_directories(orderProcessorServer PRIVATE /usr/local/include)
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

QuickFIX's `Config23.h` injects `boost::container::flat_map` into `namespace std` as a polyfill (to work around GCC 15's `std::flat_map` bugs). GCC 15 also provides native `std::flat_map` via `<flat_map>`. Having both in the same translation unit causes a redefinition conflict.

The OrderProcessor resolves this with two mechanisms:

**1. PIMPL isolation (`app/FixServer.h` / `app/FixServer.cpp`):**

`FixServer` wraps `FixGateway` + `ThreadedSocketAcceptor` behind an opaque `Impl` pointer. Only `FixServer.cpp` includes QuickFIX headers (and gets the boost polyfill). `main.cpp` and all other translation units include only `FixServer.h`, which has no QuickFIX dependencies — so they use GCC 15's native `std::flat_map` without conflict.

**2. Conditional guard in `src/NLinkedTree.h`:**

```cpp
#ifndef QUICKFIX_HAS_FLAT_CONTAINERS
#include <flat_map>
#endif
```

`QUICKFIX_HAS_FLAT_CONTAINERS` is defined by QuickFIX's `Config23.h` when the boost polyfill is active. Translation units that include QuickFIX headers (test files, bench files) get the polyfill first, then `NLinkedTree.h` skips `#include <flat_map>` to avoid the class/alias conflict. Translation units without QuickFIX (`BUILD_FIX=OFF`, or core engine files) include `<flat_map>` normally.

**Rule:** Any file that includes QuickFIX headers must include them **before** any header that transitively includes `NLinkedTree.h` (e.g., `TransactionMgr.h`).

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

If QuickFIX is not installed, build from the HFT fork:

```bash
git clone https://github.com/dudleylane/claude-code-quickfix.git
cd claude-code-quickfix
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig
```

Verify:

```bash
pkg-config --modversion quickfix-hft   # → 1.17.0-hft
ls /usr/local/lib/libquickfix.so       # → symlink exists
```
