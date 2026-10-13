/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace COP
{
namespace Codec
{

// The checks every decoder makes on a record it reads (#149). A record is the codec's fields one after another, most
// followed by a '.', so a decoder that reads a field must also know the record holds it.

/// Steps over the '.' after a field, or throws `what` when the record ends there or holds anything else. The decoders
/// used to read the byte without checking that the record still had one.
inline const char *skipSeparator(const char *p, const char *end, const char *what)
{
    if ((p >= end) || ('.' != *p))
    {
        throw std::runtime_error(what);
    }
    return p + 1;
}

/// Throws unless the decoder has read the whole record. Bytes left over mean a layout this build doesn't know, and
/// reading the record as though they weren't there would misread it.
inline void expectConsumed(const char *p, const char *end, const char *record)
{
    if (p != end)
    {
        throw std::runtime_error(std::string("Invalid format of the encoded ") + record + " - " +
                                 std::to_string(end - p) + " bytes after its last field");
    }
}

/// Throws unless the record holds anything at all
inline void expectNotEmpty(size_t size, const char *record)
{
    if (0 == size)
    {
        throw std::runtime_error(std::string("Invalid format of the encoded ") + record + " - the record is empty");
    }
}

} // namespace Codec
} // namespace COP
