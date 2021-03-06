#pragma once

#include <boost/asio/read.hpp>
#include "generic_stream.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"

namespace ouinet {

template<class Stream1, class Stream2>
inline
void full_duplex(Stream1 c1, Stream2 c2, asio::yield_context yield)
{
    static const auto timeout = std::chrono::seconds(60);

    static const auto half_duplex = []( auto& in
                                      , auto& out
                                      , auto& wdog
                                      , asio::yield_context& yield)
    {
        sys::error_code ec;
        std::array<uint8_t, 2048> data;

        for (;;) {
            size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
            if (ec) break;

            asio::async_write(out, asio::buffer(data, length), yield[ec]);
            if (ec) break;

            wdog.expires_after(timeout);
        }
    };

    WatchDog wdog( c1.get_executor()
                 , timeout
                 , [&] { c1.close(); c2.close(); });

    WaitCondition wait_condition(c1.get_executor());

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(c1, c2, wdog, yield);
          });

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(c2, c1, wdog, yield);
          });

    wait_condition.wait(yield);
}

} // ouinet namespace
