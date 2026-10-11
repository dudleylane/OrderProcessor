# QuickFIX/C++ Dependency

The FIX gateway (`-DBUILD_FIX=ON`) builds against **QuickFIX 22**, from the maintained fork at
https://github.com/dudleylane/quickfix. The branch was last verified with **v22.1.0**, on 2026-10-11.

## Why the major version is pinned

The fork changes its SONAME at every major release where code built against the previous one must
not use the new library. 21.0 changed the Application callback contract; 22.0 changed what
`message_order` stores while its comparison stays inline, so headers from 21 used with the 22
library would misorder fields. CMake therefore asks for the major it was verified with:

```cmake
if(BUILD_FIX)
    find_package(quickfix 22 CONFIG REQUIRED)
endif()

# server, tests and benchmarks:
target_link_libraries(<target> PRIVATE quickfix::quickfix)
```

The package's version check accepts the same major only, so a 21 or a 23 installation fails at
configure time instead of building. The imported target `quickfix::quickfix` brings the headers,
the library and its SSL dependencies from one installation.

## Installed files

| Property | Value |
|----------|-------|
| **Library** | `/usr/local/lib/libquickfix.so.22` → `libquickfix.so.22.1.0` (SONAME `libquickfix.so.22`) |
| **Headers** | `/usr/local/include/quickfix/` (`QUICKFIX_VERSION` is in `QuickFIXVersion.h`) |
| **CMake package** | `/usr/local/lib/cmake/quickfix/` (target `quickfix::quickfix`) |
| **pkg-config** | `/usr/local/lib/pkgconfig/quickfix.pc` |
| **Data dictionaries** | `/usr/local/share/quickfix/` |
| **Links** | `libssl`, `libcrypto`, `pthread` |

Older majors (`libquickfix.so.18` to `.21`) stay installed beside it, so programs built against them
still load, but every new build of this branch uses 22.

## What the gateway uses

FIX 4.4 only:

- **Transport and sessions:** `ThreadedSocketAcceptor`, `Session`, `SessionSettings`, `SessionID`,
  `FileStore`, `FileLog`, `Application`.
- **Messages:** `NewOrderSingle`, `NewOrderMultileg` (FX swaps, with the `NoLegs` group),
  `OrderCancelRequest`, `OrderCancelReplaceRequest`, `ExecutionReport`, `OrderCancelReject`,
  `BusinessMessageReject`, `News`.
- **Settings:** `config/fix.cfg` (acceptor on port 5001, `ResetOnLogon=Y`).

The data dictionary is the project's own copy, `config/spec/FIX44.xml`. It differs from the
`FIX44.xml` that QuickFIX installs only in the types of fields 139 (MiscFeeType) and 532
(MassCancelRejectReason): CHAR here, STRING there. The gateway uses neither.

## Installing a release

Build with gcc-toolset-15, as the rest of the project is:

```bash
cd ~/quickfix && git fetch --tags && git checkout v22.1.0
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DHAVE_SSL=ON -DENABLE_TBB_ALLOCATOR=OFF \
  -DQUICKFIX_TESTS=OFF -DQUICKFIX_EXAMPLES=OFF -DHAVE_PYTHON3=OFF
cmake --build build-release -j"$(nproc)"
sudo cmake --install build-release
sudo ldconfig
```

Check it:

```bash
grep 'define QUICKFIX_VERSION ' /usr/local/include/quickfix/QuickFIXVersion.h   # 22.1.0
readlink /usr/local/lib/libquickfix.so                                         # libquickfix.so.22
ldconfig -p | grep libquickfix.so.22
```

Then rebuild this branch from a clean build directory. A new major has a new SONAME, so nothing
built against the old one picks it up.

## Taking a new major

1. Read the release's library diff (`git diff vOLD vNEW -- src/C++`), not only its notes, for the
   classes listed above.
2. Install it as above, raise the version in `find_package(quickfix 22 CONFIG REQUIRED)`, and raise
   `QUICKFIX_REF` in `docker/Dockerfile.oms-server`, whose builder stage builds QuickFIX itself (#132).
3. Verify with FIX on: the pre-push hook (Release, `ctest -j1`, clang-format), ASan+UBSan and TSan
   as `sanitizers.yml` runs them, and a live FIX session (logon, orders that cross, a cancel, a
   two-leg FX swap) against `orderProcessorServer --fix-cfg`.

22.1.0 was taken that way. 22.0 changed three things the gateway reaches: how `message_order`
looks up a field's position (an array for compact field numbers, as FIX 4.4's are, and a hash
table otherwise, with the same ordering rule), the parsing of `SocketAcceptPort` (now checked as
0 to 65535 instead of truncated to `short`), and how data dictionaries load (faster, same
result). 22.1.0 changed only the Ruby binding.
