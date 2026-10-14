#pragma once

#include <optional>
#include <string>
#include <vector>

namespace COP
{
namespace App
{

/// What seedData is asked to do
struct SeedDataArgs
{
    std::string dataDir = "./data";
    /// --if-empty: seed only a directory with no instrument yet, and leave one in use alone, since what seedData adds is
    /// test data (#161). A directory a server ran on before it was seeded has an LMDB file, but no instrument.
    bool ifEmpty = false;
};

/// The data directory seedData seeds: `--data-dir DIR`, as orderProcessorServer takes it, or a lone DIR; "./data" when
/// neither is given. And --if-empty. Anything else returns nullopt with the reason in *error. Before #42 any first
/// argument was the directory, so `seedData --data-dir /data` seeded a directory named "--data-dir".
inline std::optional<SeedDataArgs> parseSeedDataArgs(const std::vector<std::string> &args, std::string *error)
{
    SeedDataArgs parsed;
    bool given = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        std::string dir;
        if ("--if-empty" == arg)
        {
            parsed.ifEmpty = true;
            continue;
        }
        if ("--data-dir" == arg)
        {
            if (i + 1 >= args.size())
            {
                *error = "--data-dir needs a directory";
                return std::nullopt;
            }
            dir = args[++i];
        }
        else if (!arg.empty() && '-' == arg[0])
        {
            *error = "unknown option: " + arg;
            return std::nullopt;
        }
        else
        {
            dir = arg;
        }
        if (given)
        {
            *error = "the data directory is given more than once";
            return std::nullopt;
        }
        parsed.dataDir = dir;
        given = true;
    }
    return parsed;
}

} // namespace App
} // namespace COP
