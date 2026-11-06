/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

// The benchmarks don't build with GoogleTest. orderProcessorBench links none, and its compile has no path to
// FetchContent's, so an include of <gtest/gtest.h>, which TestAux.h makes, found the host's own headers: a gtest-devel
// package that a clean host lacks, of a version the tests don't use (#530). bench/CMakeLists.txt puts this directory
// first on the benchmarks' include path, so such an include stops here, on every host. A benchmark that needs a test
// helper includes a header that doesn't include GoogleTest, as DummyOrderSaver.h is for test::DummyOrderSaver.
#error "A benchmark included <gtest/gtest.h>, which the benchmarks don't build with (#530)"
