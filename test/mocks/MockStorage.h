/**
 Concurrent Order Processor library - Google Mock Definitions

 Authors: dudleylane, Claude
 Test Infrastructure: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gmock/gmock.h>
#include "FileStorageDef.h"
#include "DataModelDef.h"

namespace test {

// Type aliases for convenience
using COP::IdT;
using COP::u32;
using COP::StringT;
using COP::RawDataEntry;
using COP::ExecutionsT;

/**
 * Mock for FileStorageObserver interface - receives storage load events
 */
class MockFileStorageObserver : public COP::Store::FileStorageObserver {
public:
    MOCK_METHOD(void, startLoad, (), (override));
    MOCK_METHOD(void, onRecordLoaded, (const IdT& id, u32 version, const char *buf, size_t size), (override));
    MOCK_METHOD(void, finishLoad, (), (override));
};

/**
 * Mock for FileSaver interface - handles file persistence operations
 */
class MockFileSaver : public COP::Store::FileSaver {
public:
    MOCK_METHOD(void, load, (const std::string &fileName, COP::Store::FileStorageObserver *observer), (override));
    MOCK_METHOD(IdT, save, (const char *buf, size_t size), (override));
    MOCK_METHOD(void, save, (const IdT &id, const char *buf, size_t size), (override));
    MOCK_METHOD(u32, update, (const IdT& id, const char *buf, size_t size), (override));
    MOCK_METHOD(u32, replace, (const IdT& id, u32 version, const char *buf, size_t size), (override));
    MOCK_METHOD(void, erase, (const IdT& id, u32 version), (override));
    MOCK_METHOD(void, erase, (const IdT& id), (override));
};

/**
 * Mock for DataSaver interface - saves domain entities to storage
 */
class MockDataSaver : public COP::Store::DataSaver {
public:
    MOCK_METHOD(void, save, (const COP::InstrumentEntry &val), (override));
    MOCK_METHOD(void, save, (const IdT& id, const StringT &val), (override));
    MOCK_METHOD(void, save, (const RawDataEntry &val), (override));
    MOCK_METHOD(void, save, (const COP::AccountEntry &val), (override));
    MOCK_METHOD(void, save, (const COP::ClearingEntry &val), (override));
    MOCK_METHOD(void, save, (const ExecutionsT &val), (override));
};

/**
 * Mock for DataStorageRestore interface - restores domain entities from storage
 */
class MockDataStorageRestore : public COP::Store::DataStorageRestore {
public:
    MOCK_METHOD(void, restore, (COP::InstrumentEntry *val), (override));
    MOCK_METHOD(void, restore, (const IdT& id, StringT *val), (override));
    MOCK_METHOD(void, restore, (RawDataEntry *val), (override));
    MOCK_METHOD(void, restore, (COP::AccountEntry *val), (override));
    MOCK_METHOD(void, restore, (COP::ClearingEntry *val), (override));
    MOCK_METHOD(void, restore, (ExecutionsT *val), (override));
};

} // namespace test
