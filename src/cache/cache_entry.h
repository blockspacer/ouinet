#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast.hpp>
#include "../namespaces.h"

namespace ouinet {

struct CacheEntry {
    using Response = http::response<http::dynamic_body>;

    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;

    // Injection identifier, empty on errors.
    std::string injection_id;

    // Cached data.
    Response response;
};

} // namespace
