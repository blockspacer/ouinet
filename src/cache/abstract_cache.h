#pragma once

#include "cache_entry.h"
#include "../util/yield.h"
#include "../generic_stream.h"
#include "../session.h"

namespace ouinet {

class AbstractCache {
public:
    virtual
    Session load(const std::string& key, Cancel, Yield) = 0;

    virtual
    void store( const std::string& key
              , Session&
              , Cancel
              , asio::yield_context) = 0;

    // Get the newest protocol version that has been seen in the network
    // (e.g. to warn about potential upgrades).
    virtual
    unsigned get_newest_proto_version() const = 0;
};

} // namespace
