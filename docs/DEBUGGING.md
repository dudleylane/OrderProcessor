# Debugging, Profiling & Production Tracing

Reference material extracted from `CLAUDE.md` to keep that file under the size threshold Claude Code applies on every turn. Use this when actively debugging, profiling, or tracing — not on every change.

## Tooling

### Sanitizer Builds

```bash
# ASan + UBSan (memory errors, buffer overflows, undefined behavior)
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

# TSan (data race detection — cannot combine with ASan)
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
```

### perf Profiling

```bash
# Record hot-path execution (requires perf_event_paranoid <= 1)
perf record -g ./build/orderProcessorBench --benchmark_filter="EventProcessing"

# Analyze call graph
perf report --sort=dso,symbol

# Cache statistics (L1/L2/LLC misses — verify false-sharing fixes)
perf stat -e cache-references,cache-misses,L1-dcache-load-misses,branch-misses,instructions ./build/orderProcessorBench

# Flamegraph workflow
perf record -F 99 -g --call-graph dwarf ./build/orderProcessorBench --benchmark_filter="EventProcessing" -- sleep 10
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Valgrind

```bash
# Cache behavior analysis (not for latency — 10-50x slowdown)
valgrind --tool=cachegrind ./build/orderProcessorBench --benchmark_filter="Pool"

# Call graph profiling
valgrind --tool=callgrind ./build/orderProcessorBench --benchmark_filter="Pool"
```

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
# Verify _mm_pause() emits PAUSE instruction in CAS loops
objdump -d build/liborderEngine.a | grep -A5 pause

# Check for unexpected heap allocations (operator new calls in hot path)
objdump -d build/liborderEngine.a | grep -B5 'operator new'

# Verify LTO inlining worked
nm build/orderProcessorBench | grep -c 'TransactionScopePool'

# Review hot functions in Compiler Explorer (godbolt.org) to verify codegen
# Paste isolated hot-path functions to check for unexpected spills, branches, or calls
```

### Fuzzing

Use libFuzzer harnesses for all untrusted input boundaries (message parsing, config loading, FIX/ITCH decoders):

```cpp
// test/fuzz/fuzz_event_parser.cpp
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Parse untrusted input
    return 0;
}
```

```bash
# Build with libFuzzer + ASan (Clang only)
cmake -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_BUILD_TYPE=Debug

# Run fuzzer
./build-fuzz/test/fuzz/fuzz_event_parser corpus/ -max_len=4096 -jobs=$(nproc)

# AFL++ alternative (for longer campaigns)
afl-fuzz -i seeds/ -o findings/ -- ./build-fuzz/test/fuzz/fuzz_event_parser @@
```

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

Production builds use `-O3`. For crash-debuggable builds, retain optimizations with full debug info:

```bash
cmake -B build-crash -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-O2 -g3 -fno-omit-frame-pointer -fno-inline-small-functions"
```

- `-g3` provides full debug info including macro definitions
- `-fno-omit-frame-pointer` ensures complete stack traces
- `-fno-inline-small-functions` keeps function names visible in backtraces
- Keep `-O2` (not `-O0`) to reproduce optimization-dependent crashes

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

# ASan with core dump on error (instead of just aborting)
ASAN_OPTIONS="abort_on_error=1:disable_coredump=0:unmap_shadow_on_exit=1" \
  ./build/orderProcessorTest

# Detect use-after-free in pool allocators — enable quarantine
ASAN_OPTIONS="quarantine_size_mb=256:abort_on_error=1" \
  ./build/orderProcessorTest

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

```bash
# Trace Processor event handling
(gdb) dprintf src/Processor.cpp:87, "onEvent: orderId=%lu status=%d\n", evnt.order_->orderId_, evnt.order_->status_
(gdb) dprintf src/Processor.cpp:110, "process_event complete, state persisted\n"

# Output to file (avoid console overhead)
(gdb) set dprintf-style call
(gdb) set dprintf-function fprintf
(gdb) set dprintf-channel stderr

# Breakpoint commands for complex logic (conditional logging, counters)
(gdb) break src/Processor.cpp:87
(gdb) commands
> silent
> printf "onEvent: orderId=%lu\n", evnt.order_->orderId_
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
