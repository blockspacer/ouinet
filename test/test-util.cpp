#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <async_sleep.h>
#include <util/async_generator.h>
#include <iostream>
#include <chrono>

BOOST_AUTO_TEST_SUITE(ouinet_util)

using namespace std;
using namespace ouinet;
using namespace chrono;
using namespace chrono_literals;
using Timer = boost::asio::steady_timer;
using Clock = chrono::steady_clock;

int millis_since(Clock::time_point start) {
    auto end = Clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

BOOST_AUTO_TEST_CASE(test_cancel) {
    using namespace chrono_literals;


    {
        asio::io_service ios;

        asio::spawn(ios, [&] (asio::yield_context yield) {
                sys::error_code ec;
                Cancel cancel;

                auto start = Clock::now();

                asio::spawn(ios, [&] (asio::yield_context yield) {
                    ios.post(yield);
                    cancel();
                });

                BOOST_REQUIRE(!cancel);
                async_sleep(ios, 1s, cancel, yield[ec]);
                BOOST_REQUIRE(millis_since(start) < 100);
        });

        ios.run();
    }

    {
        asio::io_service ios;

        asio::spawn(ios, [&] (asio::yield_context yield) {
                sys::error_code ec;
                Cancel c1;
                Cancel c2 = c1;

                auto start = Clock::now();

                asio::spawn(ios, [c1 = move(c1), &ios]
                                 (asio::yield_context yield) mutable {
                    ios.post(yield);
                    c1();
                });

                BOOST_REQUIRE(!c1);
                BOOST_REQUIRE(!c2);
                async_sleep(ios, 1s, c2, yield[ec]);
                BOOST_REQUIRE(millis_since(start) < 100);
        });
    }

    {
        asio::io_service ios;

        asio::spawn(ios, [&] (asio::yield_context yield) {
                sys::error_code ec;
                Cancel c;

                {
                    Cancel cc = c;
                }

                c();
        });

        ios.run();
    }

    {
        Cancel parent;
        unique_ptr<Cancel> child1(new Cancel(parent));
        Cancel child2(std::move(*child1));
        child1 = nullptr;
        parent();
    }
}

BOOST_AUTO_TEST_CASE(test_async_generator) {
    {
        using util::AsyncGenerator;

        asio::io_service ios;

        asio::spawn(ios, [&] (asio::yield_context yield) {
            AsyncGenerator<int> gen(ios, [&] (auto& q, auto c, auto y) {
                q.push_back(1);
            });

            Cancel cancel;
            auto opt_val = gen.async_get_value(cancel, yield);

            BOOST_REQUIRE(opt_val && *opt_val == 1);
        });

        ios.run();
    }

    {
        using util::AsyncGenerator;

        asio::io_service ios;

        asio::spawn(ios, [&] (asio::yield_context yield) {
            AsyncGenerator<int> gen(ios, [&] (auto& q, auto c, auto y) {
                ios.post(y);
                q.push_back(1);
                ios.post(y);
                if (c) or_throw(y, asio::error::operation_aborted);
            });

            Cancel cancel;
            auto opt_val = gen.async_get_value(cancel, yield);

            BOOST_REQUIRE(opt_val && *opt_val == 1);
        });

        ios.run();
    }
}

BOOST_AUTO_TEST_SUITE_END()

