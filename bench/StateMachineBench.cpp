#include <benchmark/benchmark.h>
#include "StateMachine.h"
#include "OrderStateMachineImpl.h"

// Placeholder benchmark - will be populated during test migration
static void BM_StateMachineTransitions(benchmark::State& state) {
    for (auto _ : state) {
        // TODO: Implement state machine transitions benchmark
        benchmark::DoNotOptimize(state.iterations());
    }
}
BENCHMARK(BM_StateMachineTransitions)->Range(8, 8 << 10);
