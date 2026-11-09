#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Logger.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"
#include "ReferenceDataFile.h"
#include "SeedDataArgs.h"
#include "ServerDefaults.h"

using namespace COP;

namespace
{
const char *const USAGE = "usage: seedData [--if-empty] [--file FILE] [--data-dir DIR | DIR]   (default ./data)\n"
                          "  --if-empty   seed only a directory with no instrument yet (#161)\n"
                          "  --file FILE  load FILE's instruments, accounts and clearing firms, and not the test set\n"
                          "               (#585); README gives the format\n";

/// What seedData seeds without --file: test data, so not for a directory in real use
App::ReferenceData testSet()
{
    App::ReferenceData data;
    struct InstrDef
    {
        const char *symbol;
        const char *secId;
        const char *secIdSrc;
    };
    InstrDef instruments[] = {
        { "AAPL", "US0378331005", "ISIN" }, { "MSFT", "US5949181045", "ISIN" }, { "GOOGL", "US02079K3059", "ISIN" },
        { "AMZN", "US0231351067", "ISIN" }, { "TSLA", "US88160R1014", "ISIN" }, { "META", "US30303M1027", "ISIN" },
        { "NVDA", "US67066G1040", "ISIN" }, { "JPM", "US46625H1005", "ISIN" },  { "V", "US92826C8394", "ISIN" },
        { "JNJ", "US4781601046", "ISIN" },
    };
    for (auto &def : instruments)
    {
        InstrumentEntry instr;
        instr.symbol_ = def.symbol;
        instr.securityId_ = def.secId;
        instr.securityIdSource_ = def.secIdSrc;
        data.instruments.push_back(instr);
    }

    struct AcctDef
    {
        const char *name;
        const char *firm;
        AccountType type;
    };
    // TRADING-1 is the server's default account, App::DEFAULT_ACCOUNT (#34)
    AcctDef accounts[] = {
        { "TRADING-1", "Apex Capital", PRINCIPAL_ACCOUNTTYPE }, { "TRADING-2", "Apex Capital", PRINCIPAL_ACCOUNTTYPE },
        { "CLIENT-A", "Summit Partners", AGENCY_ACCOUNTTYPE },  { "CLIENT-B", "Meridian Fund", AGENCY_ACCOUNTTYPE },
        { "PROP-DESK", "Apex Capital", PRINCIPAL_ACCOUNTTYPE },
    };
    for (auto &def : accounts)
    {
        AccountEntry acct;
        acct.account_ = def.name;
        acct.firm_ = def.firm;
        acct.type_ = def.type;
        data.accounts.push_back(acct);
    }

    // The clearing firm the server puts on every order (#34)
    ClearingEntry clearing;
    clearing.firm_ = App::DEFAULT_CLEARING_FIRM;
    data.clearingFirms.push_back(clearing);
    return data;
}

/// What the file gives that's stored with other fields: seedData adds what isn't stored, and changes nothing that is
/// (#585). Orders refer to their instrument and account, and the server reads reference data only as it starts.
std::vector<std::string> storedOtherwise(const App::ReferenceData &data, const Store::WideParamsDataStorage &stored)
{
    std::vector<std::string> found;
    for (const InstrumentEntry &instr : data.instruments)
    {
        const SourceIdT id = stored.findInstrumentBySymbol(instr.symbol_);
        if (!id.isValid())
        {
            continue;
        }
        InstrumentEntry was;
        stored.get(id, &was);
        if (!App::sameFields(was, instr))
        {
            found.push_back("instrument " + instr.symbol_ + " is stored with " + App::describe(was) +
                            ", and the file gives " + App::describe(instr));
        }
    }
    for (const AccountEntry &acct : data.accounts)
    {
        const SourceIdT id = stored.findAccountByName(acct.account_);
        if (!id.isValid())
        {
            continue;
        }
        AccountEntry was;
        stored.get(id, &was);
        if (!App::sameFields(was, acct))
        {
            found.push_back("account " + acct.account_ + " is stored with " + App::describe(was) +
                            ", and the file gives " + App::describe(acct));
        }
    }
    return found;
}
} // namespace

int main(int argc, char *argv[])
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    if ((1 == args.size()) && (("--help" == args[0]) || ("-h" == args[0])))
    {
        std::cout << USAGE;
        return 0;
    }
    std::string error;
    const std::optional<App::SeedDataArgs> parsed = App::parseSeedDataArgs(args, &error);
    if (!parsed)
    {
        std::cerr << "seedData: " << error << "\n" << USAGE;
        return 2;
    }
    const std::string dataDir = parsed->dataDir;

    // The file is read and checked whole before the directory is opened, so a file that's refused writes nothing (#585)
    std::optional<App::ReferenceData> data;
    if (parsed->file.empty())
    {
        data = testSet();
    }
    else
    {
        data = App::readReferenceDataFile(parsed->file, &error);
        if (!data)
        {
            std::cerr << "seedData: " << error << "\nseedData: nothing is written\n";
            return 1;
        }
    }

    // 1. Create singletons
    aux::ExchLogger::create();
    aux::ExchLogger::instance()->setNoteOn(true);
    aux::ExchLogger::instance()->setDebugOn(false); // as the server does by default (#76)
    Store::WideDataStorage::create();
    IdTGenerator::create();

    // 2. Create LMDB storage and dispatcher
    auto lmdbStorage = std::make_unique<Store::LMDBStorage>();
    auto dispatcher = std::make_unique<Store::StorageRecordDispatcher>();
    dispatcher->init(Store::WideDataStorage::instance(), nullptr, lmdbStorage.get(), nullptr);

    // WideDataStorage goes first, while the dispatcher it is bound to still exists: it never calls its saver while being
    // destroyed, and a bound storage cannot be unbound, since bindStorage() binds once and asserts against nullptr (#41).
    const auto cleanup = [&]()
    {
        IdTGenerator::destroy();
        Store::WideDataStorage::destroy();
        dispatcher.reset();
        lmdbStorage.reset();
        aux::ExchLogger::destroy();
    };

    // Load existing data first (so IDs don't collide)
    lmdbStorage->load(dataDir, dispatcher.get());
    aux::ExchLogger::instance()->note("Existing data loaded");

    if (parsed->ifEmpty)
    {
        bool hasInstrument = false;
        Store::WideDataStorage::instance()->forEachInstrument(
            [&hasInstrument](const SourceIdT &, const InstrumentEntry &)
            {
                hasInstrument = true;
            });
        if (hasInstrument)
        {
            std::cout << "seedData: " << dataDir << " has instruments already, so it isn't seeded (--if-empty)\n";
            cleanup();
            return 0;
        }
    }

    // The test set's entries are skipped by name, as before #585; the file's must be stored as the file gives them
    if (!parsed->file.empty())
    {
        const std::vector<std::string> otherwise = storedOtherwise(*data, *Store::WideDataStorage::instance());
        if (!otherwise.empty())
        {
            for (const std::string &line : otherwise)
            {
                std::cerr << "seedData: " << line << "\n";
            }
            std::cerr << "seedData: nothing is written: seedData adds what isn't stored, and changes nothing that is\n";
            cleanup();
            return 1;
        }
    }

    // Bind storage so new adds get persisted
    Store::WideDataStorage::instance()->bindStorage(dispatcher.get());

    // 3. Add instruments
    int instrCount = 0;
    for (const InstrumentEntry &def : data->instruments)
    {
        // Check if already exists
        if (Store::WideDataStorage::instance()->findInstrumentBySymbol(def.symbol_).isValid())
        {
            std::cout << "  Instrument " << def.symbol_ << " already exists, skipping\n";
            continue;
        }
        SourceIdT id = Store::WideDataStorage::instance()->add(new InstrumentEntry(def));
        std::cout << "  Added instrument: " << def.symbol_ << " (id=" << id.id_ << ")\n";
        instrCount++;
    }

    // 4. Add accounts
    int acctCount = 0;
    for (const AccountEntry &def : data->accounts)
    {
        if (Store::WideDataStorage::instance()->findAccountByName(def.account_).isValid())
        {
            std::cout << "  Account " << def.account_ << " already exists, skipping\n";
            continue;
        }
        SourceIdT id = Store::WideDataStorage::instance()->add(new AccountEntry(def));
        std::cout << "  Added account: " << def.account_ << " @ " << def.firm_ << " (id=" << id.id_ << ")\n";
        acctCount++;
    }

    // 5. Add clearing firms
    int clearingCount = 0;
    for (const ClearingEntry &def : data->clearingFirms)
    {
        if (Store::WideDataStorage::instance()->findClearingByFirm(def.firm_).isValid())
        {
            std::cout << "  Clearing firm " << def.firm_ << " already exists, skipping\n";
            continue;
        }
        SourceIdT id = Store::WideDataStorage::instance()->add(new ClearingEntry(def));
        std::cout << "  Added clearing firm: " << def.firm_ << " (id=" << id.id_ << ")\n";
        clearingCount++;
    }

    std::cout << "\nSeeded " << instrCount << " instruments, " << acctCount << " accounts and " << clearingCount
              << " clearing firms into " << dataDir;
    if (!parsed->file.empty())
    {
        std::cout << " from " << parsed->file;
    }
    std::cout << "\n";

    // 6. Cleanup
    cleanup();
    return 0;
}
