#include <iostream>
#include <memory>
#include <string>

#include "Logger.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"

using namespace COP;

int main(int argc, char* argv[]) {
    std::string dataDir = "./data";
    if (argc > 1)
        dataDir = argv[1];

    // 1. Create singletons
    aux::ExchLogger::create();
    aux::ExchLogger::instance()->setNoteOn(true);
    Store::WideDataStorage::create();
    IdTGenerator::create();

    // 2. Create LMDB storage and dispatcher
    auto lmdbStorage = std::make_unique<Store::LMDBStorage>();
    auto dispatcher = std::make_unique<Store::StorageRecordDispatcher>();
    dispatcher->init(Store::WideDataStorage::instance(), nullptr, lmdbStorage.get(), nullptr);

    // Load existing data first (so IDs don't collide)
    lmdbStorage->load(dataDir, dispatcher.get());
    aux::ExchLogger::instance()->note("Existing data loaded");

    // Bind storage so new adds get persisted
    Store::WideDataStorage::instance()->bindStorage(dispatcher.get());

    // 3. Add instruments
    struct InstrDef { const char* symbol; const char* secId; const char* secIdSrc; };
    InstrDef instruments[] = {
        {"AAPL",  "US0378331005", "ISIN"},
        {"MSFT",  "US5949181045", "ISIN"},
        {"GOOGL", "US02079K3059", "ISIN"},
        {"AMZN",  "US0231351067", "ISIN"},
        {"TSLA",  "US88160R1014", "ISIN"},
        {"META",  "US30303M1027", "ISIN"},
        {"NVDA",  "US67066G1040", "ISIN"},
        {"JPM",   "US46625H1005", "ISIN"},
        {"V",     "US92826C8394", "ISIN"},
        {"JNJ",   "US4781601046", "ISIN"},
    };

    int instrCount = 0;
    for (auto& def : instruments) {
        // Check if already exists
        if (Store::WideDataStorage::instance()->findInstrumentBySymbol(def.symbol).isValid()) {
            std::cout << "  Instrument " << def.symbol << " already exists, skipping\n";
            continue;
        }
        auto* instr = new InstrumentEntry();
        instr->symbol_ = def.symbol;
        instr->securityId_ = def.secId;
        instr->securityIdSource_ = def.secIdSrc;
        SourceIdT id = Store::WideDataStorage::instance()->add(instr);
        std::cout << "  Added instrument: " << def.symbol << " (id=" << id.id_ << ")\n";
        instrCount++;
    }

    // 4. Add accounts
    struct AcctDef { const char* name; const char* firm; AccountType type; };
    AcctDef accounts[] = {
        {"TRADING-1",  "Apex Capital",      PRINCIPAL_ACCOUNTTYPE},
        {"TRADING-2",  "Apex Capital",      PRINCIPAL_ACCOUNTTYPE},
        {"CLIENT-A",   "Summit Partners",   AGENCY_ACCOUNTTYPE},
        {"CLIENT-B",   "Meridian Fund",     AGENCY_ACCOUNTTYPE},
        {"PROP-DESK",  "Apex Capital",      PRINCIPAL_ACCOUNTTYPE},
    };

    int acctCount = 0;
    for (auto& def : accounts) {
        if (Store::WideDataStorage::instance()->findAccountByName(def.name).isValid()) {
            std::cout << "  Account " << def.name << " already exists, skipping\n";
            continue;
        }
        auto* acct = new AccountEntry();
        acct->account_ = def.name;
        acct->firm_ = def.firm;
        acct->type_ = def.type;
        SourceIdT id = Store::WideDataStorage::instance()->add(acct);
        std::cout << "  Added account: " << def.name << " @ " << def.firm << " (id=" << id.id_ << ")\n";
        acctCount++;
    }

    std::cout << "\nSeeded " << instrCount << " instruments and " << acctCount << " accounts into " << dataDir << "\n";

    // 5. Cleanup
    Store::WideDataStorage::instance()->bindStorage(nullptr);
    dispatcher.reset();
    lmdbStorage.reset();

    IdTGenerator::destroy();
    Store::WideDataStorage::destroy();
    aux::ExchLogger::destroy();

    return 0;
}
