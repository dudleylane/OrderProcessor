#pragma once

#include <optional>
#include <string>
#include <vector>

namespace COP
{
namespace App
{

/// The data directory seedData seeds: `--data-dir DIR`, as orderProcessorServer takes it, or a lone DIR; "./data" when
/// neither is given. Anything else returns nullopt with the reason in *error. Before #42 any first argument was the
/// directory, so `seedData --data-dir /data` seeded a directory named "--data-dir".
inline std::optional<std::string> parseSeedDataArgs(const std::vector<std::string> &args, std::string *error)
{
    std::string dataDir = "./data";
    bool given = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        std::string dir;
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
        dataDir = dir;
        given = true;
    }
    return dataDir;
}

} // namespace App
} // namespace COP
