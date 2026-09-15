/**
 Concurrent Order Processor library - standalone driver for fuzz targets

 Replaces libFuzzer's main() when the target is built without
 -fsanitize=fuzzer (GCC, or a plain regression run). Every argument is a file
 or a directory; each regular file is fed once to LLVMFuzzerTestOneInput().
 Exit status is non-zero only if no input was run; a failing input crashes the
 process, exactly as it would under libFuzzer.

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace
{

void runFile(const std::filesystem::path &path, size_t &count)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    ++count;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <file-or-directory>...\n", argv[0]);
        return 2;
    }
    size_t count = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::filesystem::path arg(argv[i]);
        if (std::filesystem::is_directory(arg))
        {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(arg))
            {
                if (entry.is_regular_file())
                {
                    runFile(entry.path(), count);
                }
            }
        }
        else
        {
            runFile(arg, count);
        }
    }
    std::printf("%zu input(s) ran without a crash\n", count);
    return count == 0 ? 1 : 0;
}
