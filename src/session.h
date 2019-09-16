#pragma once

#include <boost/beast/http/buffer_body.hpp>

#include "generic_stream.h"
#include "http_forward.h"
#include "util/yield.h"

namespace ouinet {

class Session {
private:
    struct State {
        GenericStream con;
        beast::static_buffer<http_forward_block> buffer;
        http::response_parser<http::buffer_body> parser;
        boost::optional<bool> response_keep_alive;

        State(GenericStream&& con) : con(std::move(con)) {
            // Allow an unlimited body size (not kept in memory).
            parser.body_limit((std::numeric_limits<std::size_t>::max)());
        }
    };

public:
    Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    Session(GenericStream con)
        : _state(new State(std::move(con)))
    {}

    http::response_header<>* response_header() const {
        if (!_state) return nullptr;
        if (!_state->parser.is_header_done()) return nullptr;
        return &_state->parser.get().base();
    }

    http::response_header<>* read_response_header(Cancel&, asio::yield_context);

    template<class SinkStream>
    void flush_response(SinkStream&, Cancel&, asio::yield_context);

    // Allows manipulating the head, body and trailer forwarded to the sink.
    template< class SinkStream, class ProcHead, class ProcIn
            , class ProcTrail, class ProcChkExt>
    void flush_response( SinkStream&
                       , ProcHead, ProcIn, ProcTrail, ProcChkExt
                       , Cancel&, asio::yield_context);

    // Loads the entire response to memory, use only for debugging
    template<class BodyType>
    http::response<BodyType> slurp(Cancel&, asio::yield_context);

    bool is_open() const {
        return _state->con.is_open();
    }

    void close() {
        if (!_state) return;
        if (_state->con.is_open()) _state->con.close();
    }

    void keep_alive(bool v) {
        assert(_state);
        if (!_state) return;
        _state->response_keep_alive = v;
    }

    bool keep_alive() const {
        assert(_state);
        if (!_state) return false;
        return _state->parser.get().keep_alive();
    }

private:
    std::unique_ptr<State> _state;
};

inline
http::response_header<>*
Session::read_response_header(Cancel& cancel, asio::yield_context yield)
{
    assert(!cancel);

    if (!_state) {
        return or_throw<http::response_header<>*>(
                yield, asio::error::bad_descriptor);
    }

    if (auto h = response_header()) return h;

    auto c = cancel.connect([&] { _state->con.close(); });

    sys::error_code ec;
    http::async_read_header(_state->con,
                            _state->buffer,
                            _state->parser,
                            yield[ec]);

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<http::response_header<>*>(yield, ec, nullptr);

    assert(_state->parser.is_header_done());

    return response_header();
}

template<class SinkStream, class ProcHead, class ProcIn, class ProcTrail, class ProcChkExt>
inline
void
Session::flush_response(SinkStream& sink,
                        ProcHead hproc, ProcIn dproc, ProcTrail tproc, ProcChkExt xproc,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    if (!_state) {
        return or_throw(yield, asio::error::bad_descriptor);
    }

    Yield yield_(sink.get_io_service(), yield, "flush_response");
    http_forward( _state->con, sink, _state->buffer, _state->parser
                , std::move(hproc), std::move(dproc), std::move(tproc), std::move(xproc)
                , cancel, yield_);
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    // TODO: Refactor with Proxy mechanism forwarding.
    // Just pass head, chunk extensions, body data and trailer on.
    std::string chunk_exts;
    auto hproc = [] (auto inh, auto&, auto) { return inh; };
    ProcInFunc<asio::const_buffer> dproc = [&chunk_exts] (auto ind, auto&, auto) {
        ProcInFunc<asio::const_buffer>::result_type ret;
        if (asio::buffer_size(ind) > 0) {
            ret = {std::move(ind), std::move(chunk_exts)};
            chunk_exts = {};  // only send extensions in first output chunk
        }  // keep extensions when last chunk (size 0) was received
        return ret;
    };
    ProcTrailFunc tproc = [&chunk_exts] (auto intr, auto&, auto) {
        ProcTrailFunc::result_type ret{std::move(intr), std::move(chunk_exts)};
        return ret;  // leave trailers untouched
    };
    auto xproc = [&chunk_exts] (auto exts, auto&, auto) {
        chunk_exts = std::move(exts);  // save exts for next chunk
    };

    return flush_response( sink
                         , std::move(hproc), std::move(dproc)
                         , std::move(tproc), std::move(xproc)
                         , cancel, yield );
}

template<class BodyType>
http::response<BodyType> Session::slurp(Cancel& cancel, asio::yield_context yield)
{
    if (!_state) {
        return or_throw<http::response<BodyType>>(
                yield, asio::error::bad_descriptor);
    }

    auto c = cancel.connect([&] { _state->con.close(); });

    sys::error_code ec;

    read_response_header(cancel , yield[ec]); // Won't read if already read.
    return_or_throw_on_error(yield, cancel, ec, http::response<BodyType>{});

    http::response<BodyType> rs{*response_header()};

    char buf[2048];

    std::string s;
    while (!_state->parser.is_done()) {
        _state->parser.get().body().data = buf;
        _state->parser.get().body().size = sizeof(buf);

        http::async_read(_state->con, _state->buffer, _state->parser, yield[ec]);

        if (ec == http::error::need_buffer) ec = {};
        return_or_throw_on_error(yield, cancel, ec, http::response<BodyType>{});

        size_t size = sizeof(buf) - _state->parser.get().body().size;
        s += std::string(buf, size);
    }

    typename BodyType::reader reader(rs, rs.body());
    reader.put(asio::buffer(s), ec);
    assert(!ec);

    return rs;
}

} // namespaces
