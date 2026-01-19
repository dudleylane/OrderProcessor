/**
 Concurrent Order Processor library - Test Main

 Provides global test setup including Logger initialization.
*/

#include <gtest/gtest.h>
#include "Logger.h"

class GlobalTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Initialize Logger singleton for all tests
        aux::ExchLogger::create();
    }

    void TearDown() override {
        // Destroy Logger singleton after all tests
        aux::ExchLogger::destroy();
    }
};

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new GlobalTestEnvironment());
    return RUN_ALL_TESTS();
}
