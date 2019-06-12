#pragma once

#include <array>

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/beast/http/parser.hpp>

#include "full_duplex_forward.h"
#include "or_throw.h"
#include "util.h"
#include "util/signal.h"
#include "util/yield.h"

#include "namespaces.h"

namespace ouinet {

// Get copy of response head from input, return response head for output.
using ProcHeadFunc = std::function<
    http::response_header<>(http::response_header<>, sys::error_code&)>;
// Get a buffer of data to be sent after processing a buffer of received data.
// The returned data must be alive while `http_forward` runs,
// The returned data will be wrapped in a single chunk
// if the output response is chunked.
// If the received data is empty, no more data is to be received.
// If the returned buffer is empty, nothing is sent.
template<class ConstBufferSequence>
using ProcInFunc = std::function<
    ConstBufferSequence(const asio::const_buffer& inbuf, sys::error_code&)>;

template<class StreamIn, class StreamOut, class Request, class ConstBufferSequence>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , Request rq
            , ProcHeadFunc rshproc
            , ProcInFunc<ConstBufferSequence> inproc
            , Cancel& cancel
            , Yield yield_)
{
    // TODO: Split and refactor with `fetch_http` if still useful.
    using ResponseH = http::response_header<>;

    Yield yield = yield_.tag("http_forward");

    auto cancelled = cancel.connect([&] { in.close(); out.close(); });

    sys::error_code ec;

    // Send the HTTP request to the input side.
    http::async_write(in, rq, yield[ec]);
    // Ignore `end_of_stream` error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream)
        ec = sys::error_code();

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to send request: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    // Receive the head of the HTTP response into a parser.
    beast::static_buffer<half_duplex_default_block> buffer;
    http::response_parser<http::empty_body> rpp;
    http::async_read_header(in, buffer, rpp, yield[ec]);

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to receive response head: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    assert(rpp.is_header_done());
    auto rp = rpp.get();

    // Get content length if non-chunked.
    size_t max_transfer;
    if (!rp.chunked()) {
        static const auto max_size_t = std::numeric_limits<std::size_t>::max();
        max_transfer = util::parse_num<size_t>( rp[http::field::content_length]
                                              , max_size_t);
        if (max_transfer == max_size_t)
            return or_throw<ResponseH>(yield, asio::error::invalid_argument);
    }

    // Send the HTTP response head (after processing).
    {
        auto rph_out(rpp.get().base());
        rph_out = rshproc(std::move(rph_out), ec);
        if (ec) yield.log("Failed to process response head: ", ec.message());
        if (ec) return or_throw<ResponseH>(yield, ec);

        // Write the head as a string to avoid the serializer adding an empty body
        // (which results in a terminating chunk if chunked).
        auto rph_outs = util::str(rph_out);
        asio::async_write(out, asio::buffer(rph_outs.data(), rph_outs.size()), yield[ec]);
    }

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to send response head: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    // Forward the body.
    // TODO: Implement chunkedness conversion according to `rph_out` above
    // (ensuring that chunked to non-chunked is not allowed).
    if (!rp.chunked()) {
        // Prepare forwarding buffer with body data already read.
        std::array<uint8_t, half_duplex_default_block> fwd_data;
        auto fwd_buffer = asio::buffer(fwd_data);
        auto fwd_buffer_dl = asio::buffer_copy(fwd_buffer, buffer.data());
        half_duplex(in, out, fwd_buffer, fwd_buffer_dl, max_transfer, yield[ec]);
        if (ec || cancelled)
            yield.log("Failed to forward response body: ", ec.message());
    } else {
        // Based on "Boost.Beast / HTTP / Chunked Encoding / Parsing Chunks" example.
        bool chunked_out = true;  // TODO: get from `rph_out` instead
        ConstBufferSequence outbuf;

        // TODO: watchdog
        auto body_cb = [&] (auto, auto body, auto& ec) {
            outbuf = inproc(asio::const_buffer(body.data(), body.size()), ec);
            if (ec) return 0ul;
            if (asio::buffer_size(outbuf) > 0)
                ec = http::error::end_of_chunk;  // not really, but similar semantics
            return body.size();  // wait for more data
        };
        rpp.on_chunk_body(body_cb);

        while (!rpp.is_done()) {  // `buffer` includes initial data on first read
            http::async_read(in, buffer, rpp, yield[ec]);
            if (ec == http::error::end_of_chunk)
                ec = {};  // just a signal that we have output to send
            if (ec || cancelled) {
               yield.log("Failed to read response body: ", ec.message());
               break;
            }

            if (asio::buffer_size(outbuf) == 0)
               continue;  // e.g. `buffer` filled but no output yet

            if (chunked_out)
                asio::async_write(out, http::make_chunk(outbuf), yield[ec]);
            else
                asio::async_write(out, outbuf, yield[ec]);
            if (ec || cancelled) {
                yield.log("Failed to send response body: ", ec.message());
                break;
            }
        }

        if (!(ec || cancelled) && chunked_out)
            // Trailers are handled outside.
            asio::async_write(out, http::make_chunk_last(), yield[ec]);
    }

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) return or_throw<ResponseH>(yield, ec);

    return rpp.release().base();
}

} // namespace ouinet
