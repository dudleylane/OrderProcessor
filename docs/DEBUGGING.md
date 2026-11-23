# Debugging, Profiling & Production Tracing

Recipes for debugging, profiling and tracing the engine and the server. Use this when actively debugging, profiling, or tracing — not on every change. Each recipe names the build it needs: a Release build (`build/`, as the pre-push hook configures it) or a sanitizer build (below).

## Tooling

### Sanitizer Builds

As CI's sanitizer jobs build them (`.github/workflows/sanitizers.yml`), from the repository's root:

```bash
# ASan + UBSan (memory errors, buffer overflows, undefined behavior). GCC's undefined group leaves out
# float-cast-overflow, the check that found #16's client-controlled narrowing, so it is named (#589).
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_BENCHMARKS=ON -DBUILD_FUZZ=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined,float-cast-overflow" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined,float-cast-overflow"
cmake --build build-asan
ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=1" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  ctest --test-dir build-asan --output-on-failure -j1

# TSan (data race detection — cannot combine with ASan). It needs a oneTBB built with -fsanitize=thread: the system's
# isn't instrumented, so TSan can't see the scheduler's hand-overs inside libtbb and reports every one as a race
# (hundreds of reports, none real). The workflow's "Build TSan-instrumented oneTBB" step builds it, at the tag that
# matches tbb-devel, under ~/.cache/orderprocessor/; point CMAKE_PREFIX_PATH at that prefix.
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_BENCHMARKS=OFF -DCMAKE_PREFIX_PATH=<instrumented oneTBB> \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan
ldd build-tsan/orderProcessorTest | grep libtbb   # must show libtbb_debug.so.12 from the instrumented prefix
TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" ctest --test-dir build-tsan --output-on-failure -j1
```

**What TSan doesn't see (#608).** A clean TSan run says nothing about these:

- **Standalone fences.** TSan can't model `std::atomic_thread_fence`, and GCC warns (`-Wtsan`) at each one it
  compiles. Every TSan build here gives 18 such warnings. All of them are Boost.Asio's `std_fenced_block`, from its
  constructor and destructor, in each translation unit that includes Asio: `app/` and the tests that include it. Asio
  fences handler invocation. A handler another thread posts to the server's io_context, as `WsOutQueues` posts from
  the TBB workers, is also queued under the scheduler's mutex, which TSan does see. Nothing in `src/` relies on a
  standalone fence. The warnings are why the TSan job builds without `-Werror` (#559, #608); a new one names its
  include chain.
- **Uninstrumented libraries.** TSan sees a library's memory accesses only if it was built with `-fsanitize=thread`.
  oneTBB is (above), and the system's other libraries (spdlog, LMDB, Boost's compiled parts, libstdc++'s own `.so`)
  aren't. TSan intercepts their pthread calls and atomics, but a race inside one of them, or between it and this
  code through memory it hands out, goes unreported. libstdc++'s time zone database is the case already met: its
  first lookups race under a lock TSan can't see, which is why every zone lookup holds `ExpiryScheduler`'s
  `zoneLock` (#244).

### perf Profiling

```bash
# Record hot-path execution (requires perf_event_paranoid <= 1). A Release build omits the frame pointer
# (-fomit-frame-pointer), so unwind with DWARF: plain -g follows frame pointers, and its call graphs stop short.
perf record --call-graph dwarf ./build/orderProcessorBench --benchmark_filter="BM_Process"

# Analyze call graph
perf report --sort=dso,symbol

# Cache statistics (L1/L2/LLC misses — verify false-sharing fixes)
perf stat -e cache-references,cache-misses,L1-dcache-load-misses,branch-misses,instructions ./build/orderProcessorBench

# Flamegraph workflow
perf record -F 99 --call-graph dwarf ./build/orderProcessorBench --benchmark_filter="BM_Process"
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Valgrind

```bash
# Cache behavior analysis (not for latency — 10-50x slowdown)
valgrind --tool=cachegrind ./build/orderProcessorBench --benchmark_filter="Pool"

# Call graph profiling
valgrind --tool=callgrind ./build/orderProcessorBench --benchmark_filter="Pool"

# Instructions per iteration in each per-order benchmark's timed span, under callgrind at N and 2N iterations,
# and with two binaries the second's less the first's: the instruction count the layout check reads (#503)
./scripts/loop-instructions.sh before/orderProcessorBench build/orderProcessorBench
```

Under valgrind, glibc's allocator doesn't behave as it does natively: callgrind's counts can include heap trimming (`systrim`, `sbrk`) that a native run doesn't do. Measuring #486, master's restatement benchmark ran `systrim` 1.44 times and `sbrk` 1.43 times an iteration under callgrind, about 200 instructions an iteration of the change's apparent cost, while natively (`strace -c` at N = 100,000 and 2N) `brk` ran equally often under both harnesses (#619). So split a span's delta by function (callgrind toggled on `EngineFixture::timeEvent`, `--inclusive=no`), and check an allocator delta natively, with `strace -c` or `perf stat`'s page faults, before reading it as a cost.

### clang-tidy

```bash
# Generate compile_commands.json first
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Run checks (suggested set for HFT codebases)
clang-tidy -p build src/Processor.cpp \
  -checks='-*,bugprone-*,performance-*,modernize-use-override,readability-const-return-type'
```

### Codegen Verification

```bash
# A Release build's objects hold LTO bytecode, not machine code, so read the linked binary, not liborderEngine.a
# Verify _mm_pause() emits PAUSE instruction in CAS loops
objdump -d build/orderProcessorBench | grep -A5 pause

# Check for unexpected heap allocations (operator new calls in hot path)
objdump -d build/orderProcessorBench | grep -B5 'operator new'

# Verify LTO inlining worked
nm build/orderProcessorBench | grep -c 'TransactionScopePool'

# Review hot functions in Compiler Explorer (godbolt.org) to verify codegen
# Paste isolated hot-path functions to check for unexpected spills, branches, or calls
```

### Where jumps fall (Skylake's JCC erratum)

A Skylake-era core, such as the CI laptop's i7-6820HQ, runs microcode for the JCC erratum that keeps a 32-byte block out of the uop cache when a jump in it crosses or ends on its boundary. So a change that only moves code can move a benchmark: #516's check made `BM_FixReplaceRequest` +6.0% at 20 x 3 with 92 more instructions a call, and -1.6% once both trees were built with the assembler's mitigation. Haswell, such as an i7-4770, and Zen aren't affected. On an affected core, a regression that is gone once both builds carry the mitigation, with each span's instructions counted (`scripts/loop-instructions.sh`), is placement, which CLAUDE.md's layout rule takes with a `Deviation:` trailer (#670).

```bash
# In each tree, before the change and after it: a bench build with the mitigation, at compile and at link, since
# LTO generates and assembles the code at link time
cmake -S . -B build-jcc -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_APP=OFF \
  -DCMAKE_CXX_FLAGS=-Wa,-mbranches-within-32B-boundaries -DCMAKE_EXE_LINKER_FLAGS=-Wa,-mbranches-within-32B-boundaries
cmake --build build-jcc --target orderProcessorBench

# The mitigation took: no conditional jump in the function the regression is in crosses or ends on a 32-byte
# boundary (FUNCTION: the start of its demangled name)
objdump -d --no-show-raw-insn -C build-jcc/orderProcessorBench | awk '/<FUNCTION/,/^$/' | python3 -c '
import re, sys
ins = [(int(m.group(1), 16), m.group(2)) for m in (re.match(r"\s*([0-9a-f]+):\s+(\S+)", l) for l in sys.stdin) if m]
print(sum(1 for (a, op), (b, _) in zip(ins, ins[1:])
          if op.startswith("j") and op != "jmp" and (b % 32 == 0 or a // 32 != (b - 1) // 32)), "on a boundary")'

# The tree before the change: its mitigated binary as the baseline. The tree after it: the comparison
scripts/benchmark-regression.sh --update-baseline --no-build --build-dir build-jcc --baseline /tmp/jcc-before.bin
scripts/benchmark-regression.sh --no-build --build-dir build-jcc --baseline /tmp/jcc-before.bin --pinned 2-5 \
  --rounds 20 --repetitions 3 --filter 'REGEX'
```

The counter, run on one engine source compiled alone, found 12 of its 194 conditional jumps on a boundary, and none with the mitigation. On a bench binary built as above, it found none in `OrderMatcher::match`, `Processor::process` or `OrderDataStorage::save`, where a Release bench binary built without the mitigation has 2, 35 and 53 (2026-11-22, on the i7-4770, a Haswell: the counts are the assembler's, whatever the host).

### Fuzzing

The WebSocket client-message parser (`app/ClientMessageParser.cpp`, the only place untrusted client input is parsed) has a libFuzzer target under `test/fuzz/`. It is built from that one translation unit plus nlohmann::json, with no engine link, so it builds under Clang even though the engine itself does not on this platform (the el10 spdlog 1.14 / fmt 11 pair fails Clang's consteval check inside spdlog's own headers). Always build it with `--target fuzzClientMessage`; a plain `cmake --build` would try to compile the engine too.

```bash
# Coverage-guided campaign (Clang + libFuzzer)
GCC15=/opt/rh/gcc-toolset-15/root/usr
cmake -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF -DBUILD_APP=OFF -DBUILD_FUZZ=ON \
  -DCMAKE_CXX_FLAGS="--gcc-install-dir=$GCC15/lib/gcc/x86_64-redhat-linux/15 -fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="--gcc-install-dir=$GCC15/lib/gcc/x86_64-redhat-linux/15 -fsanitize=address,undefined"
cmake --build build-fuzz --target fuzzClientMessage
mkdir -p /tmp/fuzz-work /tmp/fuzz-artifacts
LD_LIBRARY_PATH=$GCC15/lib64 UBSAN_OPTIONS=print_stacktrace=1 \
  ./build-fuzz/test/fuzz/fuzzClientMessage -max_total_time=600 -max_len=8192 -timeout=10 \
  -artifact_prefix=/tmp/fuzz-artifacts/ /tmp/fuzz-work test/fuzz/corpus/client_message

# One-shot replay of the seed corpus (any compiler; under GCC the target links a standalone driver, and
# float-cast-overflow is named, since GCC's undefined group leaves it out, #589)
cmake -B build-fuzz-gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF -DBUILD_APP=OFF -DBUILD_FUZZ=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined,float-cast-overflow"
cmake --build build-fuzz-gcc --target fuzzClientMessage
ctest --test-dir build-fuzz-gcc -R FuzzCorpus --output-on-failure
```

- Pass a scratch directory first and the tracked seed directory second: libFuzzer writes every new unit into the first directory, and `test/fuzz/corpus/` must stay as committed.
- Clang's `-fsanitize=undefined` includes `float-cast-overflow`; GCC's does not, so the GCC recipes here and CI's ASan job name it (#589), and a GCC replay catches an unchecked float-to-integer conversion too. The seeds `negative_and_huge_numbers.json` and `orderid_beyond_u64.json` found one in the parser (#16). Since f584255 they parse to a rejection under both compilers, and they stay in the corpus as its regression inputs.
- To add a seed, drop a file into `test/fuzz/corpus/client_message/`; the ctest entry picks it up on reconfigure. Crashes land under `-artifact_prefix`; commit a minimised reproducer to the corpus together with the fix.
- Contract under test: `parseClientMessage()` returns for every input and never throws. `WsSession::handleMessage()` calls it from a Beast read handler, which since #36 catches what escapes, logs it and goes on; the harness turns an escaping exception into an abort, so the parser's contract stays checked.

### cppcheck

Complementary to clang-tidy — catches different classes of bugs:

```bash
cppcheck --enable=all --std=c++23 --suppress=missingIncludeSystem \
  --inline-suppr -j$(nproc) --project=build/compile_commands.json \
  --output-file=cppcheck-report.xml --xml
```

### libc++ Hardening Modes

Runtime bounds checking in the standard library (Clang + libc++ builds only):

```bash
# Fast mode (production) — security-critical checks only, constant-time overhead
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST

# Extensive mode (CI) — all checks with low overhead
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE

# Debug mode (development) — all checks + internal library assertions
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG
```

---

## Crash Analysis

### Core Dump Configuration

```bash
# Enable core dumps
ulimit -c unlimited

# Set core dump pattern (requires root)
echo '/var/crash/core.%e.%p.%t' | sudo tee /proc/sys/kernel/core_pattern

# For systemd-managed systems
coredumpctl list                          # List recent crashes
coredumpctl info <pid>                    # Crash metadata
coredumpctl gdb <pid>                     # Open core in GDB directly
coredumpctl dump <pid> -o core.dump       # Export core file
```

### Build for Debuggable Crash Dumps

Production builds use `-O3`. For crash-debuggable builds, retain optimizations with full debug info. Use the build type `None`, not `RelWithDebInfo`: for `Release` and `RelWithDebInfo` the project adds `-O3`, `-flto` and `-fomit-frame-pointer` after `CMAKE_CXX_FLAGS` on the compile line, and CMake adds `-O2 -g`, so those win and the recipe's flags do nothing. With `None` the compile line carries the recipe's flags and the project's warnings alone:

```bash
cmake -B build-crash -G Ninja \
  -DCMAKE_BUILD_TYPE=None \
  -DCMAKE_CXX_FLAGS="-O2 -g3 -DNDEBUG -fno-omit-frame-pointer -fno-inline-small-functions"
```

- `-g3` provides full debug info including macro definitions
- `-fno-omit-frame-pointer` ensures complete stack traces
- `-fno-inline-small-functions` keeps function names visible in backtraces
- Keep `-O2` (not `-O0`) to reproduce optimization-dependent crashes, and `-DNDEBUG` to compile what a Release build compiles

### GDB Post-Mortem Workflow

```bash
# Load core dump
gdb ./build/orderProcessorServer /var/crash/core.orderProcessorServer.12345.1710000000

# Essential commands
(gdb) bt                    # Full backtrace of crashing thread
(gdb) bt full               # Backtrace with local variables
(gdb) thread apply all bt   # Backtrace of ALL threads
(gdb) info registers        # Register state at crash
(gdb) x/32gx $rsp           # Examine stack memory
(gdb) info proc mappings    # Memory map (detect corruption, unmapped access)
(gdb) frame <N>             # Select stack frame N
(gdb) info locals           # Local variables in selected frame
(gdb) disas                 # Disassembly around crash point
```

### Signal-Specific Investigation

| Signal    | Common Cause                          | Investigation                                       |
|-----------|---------------------------------------|-----------------------------------------------------|
| `SIGSEGV` | Null/dangling pointer, buffer overrun | `info registers` — check faulting address; `x/` around the pointer |
| `SIGBUS`  | Misaligned access, bad mmap           | Check `alignas` on structs; verify mmap regions     |
| `SIGABRT` | `assert()`, `std::abort()`, ASan      | `bt` — look for assertion message in frame 0-2      |
| `SIGFPE`  | Integer divide-by-zero                | `info registers` — check divisor; `disas` the instruction |
| `SIGILL`  | Corrupt code, bad `-march` on wrong CPU | Verify binary matches target CPU; check for memory corruption |
| `SIGKILL` | OOM killer, watchdog timeout          | Check `dmesg` and `/var/log/messages` for OOM entries |

### Memory Corruption Debugging

```bash
# Valgrind (thorough but slow — use for offline reproduction)
valgrind --tool=memcheck --track-origins=yes --leak-check=full \
  --show-leak-kinds=all ./build/orderProcessorTest

# ASan with core dump on error (instead of just aborting): the sanitizer build's binary, not the Release one, which has
# no ASan and ignores ASAN_OPTIONS
ASAN_OPTIONS="abort_on_error=1:disable_coredump=0:unmap_shadow_on_exit=1" \
  ./build-asan/orderProcessorTest

# A larger quarantine keeps freed memory poisoned longer, so a use long after its free is caught. It covers only what
# free and delete return: the transaction scope pool, NodePool and the scopes' arenas reuse memory without telling ASan,
# so a use after a slot, node or arena block goes back to its pool reads live memory unreported (#586)
ASAN_OPTIONS="quarantine_size_mb=256:abort_on_error=1" \
  ./build-asan/orderProcessorTest

# GDB watchpoint on corrupted address (when you know WHAT is getting corrupted)
(gdb) watch *(uint64_t*)0x7ffff7001234    # Break when this address changes
(gdb) rwatch *(uint64_t*)0x7ffff7001234   # Break on read
```

### Crash Analysis Checklist

1. **Collect artifacts**: core dump, `dmesg` output, application logs, crash handler output
2. **Identify the signal**: `SIGSEGV`/`SIGBUS`/`SIGABRT` — determines investigation path
3. **Get full backtrace**: `thread apply all bt full` — check all threads, not just the crashing one
4. **Check faulting address**: null region (0x0–0xFFFF)? stack? heap? unmapped? This narrows root cause
5. **Inspect local state**: variables, struct fields, ring buffer indices in the crashing frame
6. **Correlate timestamps**: match crash time against event queue logs and order activity
7. **Reproduce under sanitizers**: ASan for memory errors, TSan for data races, UBSan for undefined behavior
8. **Check for environment issues**: OOM (`dmesg | grep -i oom`), CPU migration, NUMA misconfig

---

## Live Debugging

### GDB Dynamic Printf (dprintf)

Insert printf-style output at any location without recompilation — the direct replacement for adding print statements:

It needs a build with debug info, such as build-crash above or a sanitizer build: a Release build has none, so `evnt`
can't be read. Name a function rather than a line: lines move with every change to the file (#572 found these recipes
pointing at a class declaration and a brace). An order's id is an `IdT`, a struct of the number and a date: print its
`id_` where it has one.

```bash
# Trace Processor event handling: each new order as the processor takes it, before it is stored, so its orderId_ is
# still 0 here and its price and quantity are what tell orders apart
(gdb) dprintf 'COP::Proc::Processor::onEvent(std::string const&, COP::Queues::OrderEvent const&)', "onEvent: price=%f qty=%u status=%d\n", evnt.order_->price_, evnt.order_->orderQty_, evnt.order_->status_

# Output to file (avoid console overhead)
(gdb) set dprintf-style call
(gdb) set dprintf-function fprintf
(gdb) set dprintf-channel stderr

# Breakpoint commands for complex logic (conditional logging, counters)
(gdb) break 'COP::Proc::Processor::onEvent(std::string const&, COP::Queues::OrderEvent const&)'
(gdb) commands
> silent
> printf "onEvent: price=%f\n", evnt.order_->price_
> continue
> end

# Persist across sessions
(gdb) save breakpoints debug-session.gdb
(gdb) source debug-session.gdb
```

### rr Record/Replay Debugging

Deterministic record-and-replay with full reverse execution — eliminates "add print, rebuild, hope it reproduces":

```bash
# Record execution (1.2x overhead)
rr record ./build/orderProcessorTest --gtest_filter="ProcessorTest.*"

# Replay deterministically — same addresses, same thread interleavings every time
rr replay

# Inside replay: full GDB with reverse execution
(gdb) break src/Processor.cpp:87
(gdb) continue                      # Run forward to breakpoint
(gdb) reverse-continue              # Run BACKWARD to previous hit
(gdb) reverse-step                  # Step backward
(gdb) watch -l order->orderId_      # Watchpoint — then reverse-continue to find who changed it
(gdb) reverse-continue              # Find the exact line that modified the field
```

**When to use rr**:
- Race conditions and Heisenbugs that don't reproduce reliably
- Use-after-free or memory corruption where you need to find the *writer*, not just the crash
- Any bug where you know the symptom but not the cause — record once, explore backward

**Limitations**: single-threaded replay (records all threads but replays one at a time), requires hardware perf counters (`perf_event_paranoid <= 2`), ~2x memory overhead.

---

## Production Tracing

### bpftrace / eBPF

Trace running processes without recompilation, restarting, or print statements. Low overhead suitable for production:

```bash
# Trace Processor::onEvent calls with order details
bpftrace -e 'uprobe:./build/orderProcessorServer:COP::Proc::Processor::onEvent {
    printf("onEvent: tid=%d\n", tid);
}'

# Histogram of function latency in nanoseconds
bpftrace -e '
uprobe:./build/orderProcessorServer:COP::Proc::Processor::onEvent { @start[tid] = nsecs; }
uretprobe:./build/orderProcessorServer:COP::Proc::Processor::onEvent {
    @latency_ns = hist(nsecs - @start[tid]);
    delete(@start[tid]);
}'

# Count function calls per second
bpftrace -e 'uprobe:./build/orderProcessorServer:COP::Queues::* { @[probe] = count(); }' \
  -d 'interval:s:1 { print(@); clear(@); }'
```

**Key advantages over print statements**:
- No recompilation or restart required
- Attach/detach from running production processes
- Kernel-level efficiency — minimal overhead when probes are not firing
- DWARF-aware: reads function signatures and struct layouts from debug info
- Can trace kernel + userspace together (e.g., correlate syscalls with application functions)

### perf Tracing (Beyond Profiling)

```bash
# Trace specific function calls with timestamps
perf probe -x ./build/orderProcessorServer 'COP::Proc::Processor::onEvent'
perf record -e probe_orderProcessorServer:* -p <pid> -- sleep 60
perf script   # Timestamped trace of every call

# Dynamic tracepoints with local variables (requires -g3 debug info)
perf probe -x ./build/orderProcessorServer 'onEvent orderId=evnt.order_->orderId_:u64'
```
