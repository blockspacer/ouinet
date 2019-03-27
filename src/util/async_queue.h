#pragma once

#include <queue>
#include "condition_variable.h"
#include "../or_throw.h"

namespace ouinet { namespace util {

template<class T> class AsyncQueue {
public:
    AsyncQueue(asio::io_service& ios, size_t max_size = -1)
        : _ios(ios)
        , _max_size(max_size)
        , _rx_cv(_ios)
        , _tx_cv(_ios)
    {}

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;

    // Might be possible, but non-trivial (I think)
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    void async_push(T val, Cancel& cancel, asio::yield_context yield)
    {
        async_push(std::move(val), sys::error_code(), cancel, yield);
    }

    void async_push( T val
                   , sys::error_code ec_
                   , Cancel& cancel
                   , asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });

        sys::error_code ec;

        while (_queue.size() >= _max_size) {
            _tx_cv.wait(yield[ec]);
            if (cancel) ec = asio::error::operation_aborted;
            if (ec) return or_throw(yield, ec);
        }

        _queue.push({std::move(val), ec_});
        _rx_cv.notify();
    }

    T async_pop(Cancel& cancel, asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });

        sys::error_code ec;

        while (_queue.empty()) {
            _rx_cv.wait(yield[ec]);
            if (cancel) ec = asio::error::operation_aborted;
            if (ec) return or_throw<T>(yield, ec);
        }

        assert(!_queue.empty());

        auto ret = std::move(_queue.front());
        _queue.pop();

        _tx_cv.notify();

        return or_throw<T>(yield, ret.second, std::move(ret.first));
    }

    ~AsyncQueue()
    {
        _destroy_signal();
    }

    size_t size() const { return _queue.size(); }

private:
    asio::io_service& _ios;
    size_t _max_size;
    std::queue<std::pair<T, sys::error_code>> _queue;
    ConditionVariable _rx_cv;
    ConditionVariable _tx_cv;
    Cancel _destroy_signal;
};

}} // namespaces