# QuickFIX/C++ Dependency

The FIX gateway (`-DBUILD_FIX=ON`) builds against **QuickFIX 22**, from the maintained fork at
https://github.com/dudleylane/quickfix. The branch was last verified with **v22.1.0**, on 2026-10-11.

One ref is the source of truth: `QUICKFIX_REF` in `docker/Dockerfile.oms-server`. The image builds
it, and CI's FIX-on jobs read it and build the same ref with the same script,
`scripts/build-quickfix.sh`, so CI tests what the image ships (#178). Before #178, CI built against
whatever QuickFIX 22 the runner host had in `/usr/local`, which changes outside this repository: it
was 22.2.0 while the image shipped 22.1.0.

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

`PREFIX` is `/usr/local` in the image, and `~/.cache/orderprocessor/quickfix-REF-gccVERSION` in CI
(the runner's home, kept between jobs and rebuilt only when the ref or the compiler changes).

| Property | Value |
|----------|-------|
| **Library** | `PREFIX/lib/libquickfix.so.22` → `libquickfix.so.22.1.0` (SONAME `libquickfix.so.22`) |
| **Headers** | `PREFIX/include/quickfix/` (`QUICKFIX_VERSION` is in `QuickFIXVersion.h`) |
| **CMake package** | `PREFIX/lib/cmake/quickfix/` (target `quickfix::quickfix`) |
| **pkg-config** | `PREFIX/lib/pkgconfig/quickfix.pc` |
| **Data dictionaries** | `PREFIX/share/quickfix/` |
| **Ref** | `PREFIX/quickfix-ref`: the ref and its commit, which the script writes last |
| **Links** | `libssl`, `libcrypto`, `pthread` |

A binary built against a prefix finds the library through its RUNPATH. A host may also have older
majors (`libquickfix.so.18` to `.21`) installed for other programs; every new build of this branch
uses 22.

## What the gateway uses

FIX 4.4 only:

- **Transport and sessions:** `ThreadedSocketAcceptor`, or `ThreadedSSLSocketAcceptor` with `--fix-tls-cert` and
  `--fix-tls-key` (#228), `Session`, `SessionSettings`, `SessionID`, `FileStore`, `FileLog`, `Application`.
- **TLS (#228):** the SSL acceptor exists only in a build with `HAVE_SSL`. The pinned release's install defines it in
  `QuickFIXBuildConfig.h`, which `ThreadedSSLSocketAcceptor.h` includes, so the gateway needs no define of its own;
  `scripts/build-quickfix.sh` builds with `-DHAVE_SSL=ON`. The server puts `ServerCertificateFile`,
  `ServerCertificateKeyFile` and, unless `config/fix.cfg` sets one, `SSLProtocol=-all +TLSv1_2 +TLSv1_3` into the
  settings' `[DEFAULT]`, the only place QuickFIX's SSL code reads them (`app/FixTls.h`). QuickFIX's own default,
  `all -SSLv2`, would allow SSLv3, TLS 1.0 and TLS 1.1.
- **Messages:** `NewOrderSingle`, `NewOrderMultileg` (FX swaps, with the `NoLegs` group),
  `OrderCancelRequest`, `OrderCancelReplaceRequest`, `ExecutionReport`, `OrderCancelReject`,
  `BusinessMessageReject`, `News`.
- **Settings:** `config/fix.cfg` (acceptor on port 5001, `ResetOnLogon=N`, one session a day from 17:00 New York,
  read with `UseLocalTime=Y` (#240)). It names no session: the server makes one for each counterparty in
  `--fix-credentials` (#147).
- **Logon:** `Application::fromAdmin` checks a Logon's `Username` (553) and `Password` (554) against the credentials
  file, and throws `RejectLogon` to refuse it (#147).
  The logs come from the project's `FixRotatingLogFactory` (`app/FixRotatingLog.cpp`), not `FileLog`:
  the same files and lines, rotated at `FileLogMaxMB` with `FileLogBackups` kept (#160).

The data dictionary is the project's own copy, `config/spec/FIX44.xml`. It differs from the
`FIX44.xml` that QuickFIX installs only in the types of fields 139 (MiscFeeType) and 532
(MassCancelRejectReason): CHAR here, STRING there. The gateway uses neither.

## Building against the pinned release

Build the pinned ref as CI does, with gcc-toolset-15 first on `PATH`, and point CMake at it. This
needs no root, and leaves the host's `/usr/local` alone:

```bash
REF=$(sed -n 's/^ARG QUICKFIX_REF=//p' docker/Dockerfile.oms-server | tr -d '\r')
PREFIX=~/.cache/orderprocessor/quickfix-$REF-gcc$(g++ -dumpfullversion)
[ -f "$PREFIX/quickfix-ref" ] || scripts/build-quickfix.sh "$REF" "$PREFIX"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=ON -DCMAKE_PREFIX_PATH="$PREFIX"
```

Check it: `cat "$PREFIX/quickfix-ref"` names the ref and its commit, and
`ldd build/orderProcessorTest | grep quickfix` should show a library under `$PREFIX`.

Without `CMAKE_PREFIX_PATH`, CMake takes the first QuickFIX 22 it finds, usually the host's
`/usr/local` one. So a build directory configured that way, as the pre-push hook's first configure
is, may test another 22.x than CI. Configure a fresh build directory to switch: a build directory
keeps the QuickFIX it found first (`quickfix_DIR` in its cache).

## Taking a new release

1. Read the release's library diff (`git diff vOLD vNEW -- src/C++`), not only its notes, for the
   classes listed above.
2. Raise `QUICKFIX_REF` in `docker/Dockerfile.oms-server`, the one place CI and the image take the
   ref from (#178). For a new major, also raise the version in `find_package(quickfix 22 CONFIG
   REQUIRED)`.
3. Verify with FIX on: the pre-push hook (Release, `ctest -j1`, clang-format), ASan+UBSan and TSan
   as `sanitizers.yml` runs them, and a live FIX session (logon, orders that cross, a cancel, a
   two-leg FX swap) against `orderProcessorServer --fix-cfg`.

22.1.0 was taken that way. 22.0 changed three things the gateway reaches: how `message_order`
looks up a field's position (an array for compact field numbers, as FIX 4.4's are, and a hash
table otherwise, with the same ordering rule), the parsing of `SocketAcceptPort` (now checked as
0 to 65535 instead of truncated to `short`), and how data dictionaries load (faster, same
result). 22.1.0 changed only the Ruby binding.
