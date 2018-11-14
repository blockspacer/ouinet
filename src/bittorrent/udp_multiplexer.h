#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/utility/string_view.hpp>
#include "../namespaces.h"
#include "../or_throw.h"
#include "../util/condition_variable.h"

namespace ouinet { namespace bittorrent {

static
boost::asio::const_buffers_1 buffer(const std::string& s) {
    return boost::asio::buffer(const_cast<const char*>(s.data()), s.size());
}

class UdpMultiplexer {
private:
    using udp = asio::ip::udp;

    using IntrusiveHook = boost::intrusive::list_base_hook
        <boost::intrusive::link_mode
            <boost::intrusive::auto_unlink>>;

    template<class T>
    using IntrusiveList = boost::intrusive::list
        <T, boost::intrusive::constant_time_size<false>>;

    struct SendEntry {
        std::string message;
        udp::endpoint to;
        Signal<void(sys::error_code)> sent_signal;
    };

    struct RecvEntry : IntrusiveHook {
        std::function<void(
            sys::error_code,
            boost::string_view,
            udp::endpoint
        )> handler;
    };

public:
    UdpMultiplexer(udp::socket&&);

    asio::io_service& get_io_service();

    void send(std::string&& message, const udp::endpoint& to, asio::yield_context yield, Signal<void()>& cancel_signal);
    void send(std::string&& message, const udp::endpoint& to, asio::yield_context yield)
        { Signal<void()> cancel_signal; send(std::move(message), to, yield, cancel_signal); }
    void send(std::string&& message, const udp::endpoint& to);

    // NOTE: The pointer inside the returned string_view is guaranteed to
    // be valid only until the next coroutine based async IO call or until
    // the coroutine that runs this function exits (whichever comes first).
    const boost::string_view receive(udp::endpoint& from, asio::yield_context yield, Signal<void()>& cancel_signal);
    const boost::string_view receive(udp::endpoint& from, asio::yield_context yield)
        { Signal<void()> cancel_signal; return receive(from, yield, cancel_signal); }

    ~UdpMultiplexer();

private:
    udp::socket _socket;
    std::list<SendEntry> _send_queue;
    ConditionVariable _send_queue_nonempty;
    IntrusiveList<RecvEntry> _receive_queue;
    Signal<void()> _terminate_signal;
};

inline
UdpMultiplexer::UdpMultiplexer(udp::socket&& s):
    _socket(std::move(s)),
    _send_queue_nonempty(_socket.get_io_service())
{
    assert(_socket.is_open());

    asio::spawn(get_io_service(), [this] (asio::yield_context yield) {
        bool stopped = false;
        auto terminate_slot = _terminate_signal.connect([&] {
            stopped = true;
            _send_queue_nonempty.notify();
        });

        while(true) {
            if (stopped) {
                break;
            }

            if (_send_queue.empty()) {
                sys::error_code ec;
                _send_queue_nonempty.wait(yield[ec]);
                continue;
            }

            SendEntry& entry = _send_queue.front();

            sys::error_code ec;
            _socket.async_send_to(buffer(entry.message), entry.to, yield[ec]);
            if (stopped) {
                break;
            }

            _send_queue.front().sent_signal(ec);
            _send_queue.pop_front();
        }
    });

    asio::spawn(get_io_service(), [this] (asio::yield_context yield) {
        bool stopped = false;
        auto terminate_slot = _terminate_signal.connect([&] {
            stopped = true;
        });

        std::vector<uint8_t> buf;
        udp::endpoint from;

        while (true) {
            sys::error_code ec;

            buf.resize(65536);

            size_t size = _socket.async_receive_from( asio::buffer(buf), from, yield[ec]);
            if (stopped) {
                break;
            }

            buf.resize(size);
            for (auto& entry : std::move(_receive_queue)) {
                entry.handler(ec, boost::string_view((char*)&buf[0], size), from);
            }
        }
    });
}

inline
UdpMultiplexer::~UdpMultiplexer()
{
    _terminate_signal();
    _socket.close();
}

inline
void UdpMultiplexer::send(
    std::string&& message,
    const udp::endpoint& to,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    ConditionVariable condition(get_io_service());

    sys::error_code ec;

    _send_queue.emplace_back();
    _send_queue.back().message = std::move(message);
    _send_queue.back().to = to;
    auto sent_slot = _send_queue.back().sent_signal.connect([&] (sys::error_code ec_) {
        ec = ec_;
        condition.notify();
    });

    auto cancel_slot = cancel_signal.connect([&] {
        ec = boost::asio::error::operation_aborted;
        condition.notify();
    });

    auto terminate_slot = _terminate_signal.connect([&] {
        ec = boost::asio::error::operation_aborted;
        condition.notify();
    });

    _send_queue_nonempty.notify();
    condition.wait(yield);

    if (ec) {
        or_throw(yield, ec);
    }
}

inline
void UdpMultiplexer::send(
    std::string&& message,
    const udp::endpoint& to
) {
    _send_queue.emplace_back();
    _send_queue.back().message = std::move(message);
    _send_queue.back().to = to;

    _send_queue_nonempty.notify();
}

inline
const boost::string_view
UdpMultiplexer::receive(udp::endpoint& from, asio::yield_context yield, Signal<void()>& cancel_signal)
{
    ConditionVariable condition(get_io_service());

    sys::error_code ec;
    boost::string_view buffer;

    RecvEntry recv_entry;
    recv_entry.handler = [&](sys::error_code ec_, boost::string_view buffer_, udp::endpoint from_) {
        ec = ec_;
        buffer = buffer_;
        from = from_;
        condition.notify();
    };
    _receive_queue.push_back(recv_entry);

    auto cancel_slot = cancel_signal.connect([&] {
        ec = boost::asio::error::operation_aborted;
        condition.notify();
    });

    auto terminate_slot = _terminate_signal.connect([&] {
        ec = boost::asio::error::operation_aborted;
        condition.notify();
    });

    condition.wait(yield);

    if (ec) {
        return or_throw<boost::string_view>(yield, ec);
    }

    return buffer;
}

inline
asio::io_service& UdpMultiplexer::get_io_service()
{
    return _socket.get_io_service();
}

}} // namespaces
