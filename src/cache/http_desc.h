// Temporary, simplified URI descriptor format for a single HTTP response.
//
// See `doc/descriptor-*.json` for the target format.
#pragma once

#include <sstream>

#include <boost/format.hpp>
#include <json.hpp>

#include "../namespaces.h"
#include "../or_throw.h"

namespace ouinet {

namespace descriptor {

// For the given HTTP request `rq` and response `rs`, seed body data to the `cache`,
// then create an HTTP descriptor with the given `id` for the URL and response,
// and pass it to the given callback.
template<class Cache>
inline
void
http_create( Cache& cache
           , const std::string& id
           , const http::request<http::string_body>& rq
           , const http::response<http::dynamic_body>& rs
           , std::function<void(sys::error_code, std::string)> cb) {

    // TODO: Do it more efficiently?
    cache.put_data(beast::buffers_to_string(rs.body().data()),
        [id, rq, rsh = rs.base(), cb = std::move(cb)] (const sys::error_code& ec, auto ipfs_id) {
            auto url = rq.target().to_string();
            if (ec) {
                std::cout << "!Data seeding failed: " << url << " " << id
                          << " " << ec.message() << std::endl;
                return cb(ec, "");
            }

            // Create the descriptor.
            // TODO: This is a *temporary format* with the bare minimum to test
            // head/body splitting of HTTP responses.
            std::stringstream rsh_ss;
            rsh_ss << rsh;

            nlohmann::json desc;
            desc["url"] = url;
            desc["id"] = id;
            desc["head"] = rsh_ss.str();
            desc["body_link"] = ipfs_id;

            cb(ec, std::move(desc.dump()));
        });
}

// For the given HTTP descriptor serialized in `desc_data`,
// retrieve the head from the descriptor and the body data from the `cache`,
// assemble and return the HTTP response along with its identifier.
template<class Cache>
inline
std::pair< http::response<http::dynamic_body>
         , std::string
         >
http_parse( Cache& cache, const std::string& desc_data
          , asio::yield_context yield) {

    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;
    std::string url, id, head, body_link, body;

    // Parse the JSON HTTP descriptor, extract useful info.
    try {
        auto json = nlohmann::json::parse(desc_data);
        url = json["url"];
        id = json["id"];
        head = json["head"];
        body_link = json["body_link"];
    } catch (const std::exception& e) {
        std::cerr << "WARNING: Malformed or invalid HTTP descriptor: " << e.what() << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << desc_data << std::endl;
        std::cerr << "----------------" << std::endl;
        ec = asio::error::invalid_argument;  // though ``bad_descriptor`` would rock
    }

    if (!ec)
        // Get the HTTP response body (stored independently).
        body = cache.get_data(body_link, yield[ec]);

    if (ec)
        return or_throw<std::pair<Response, std::string>>(yield, ec);

    // Build an HTTP response from the head in the descriptor and the retrieved body.
    http::response_parser<Response::body_type> parser;
    parser.eager(true);

    // - Parse the response head.
    parser.put(asio::buffer(head), ec);
    if (ec || !parser.is_header_done()) {
        std::cerr << "WARNING: Malformed or incomplete HTTP head in descriptor" << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << head << std::endl;
        std::cerr << "----------------" << std::endl;
        ec = asio::error::invalid_argument;
        return or_throw<std::pair<Response, std::string>>(yield, ec);
    }

    // - Add the response body (if needed).
    if (body.length() > 0)
        parser.put(asio::buffer(body), ec);
    else
        parser.put_eof(ec);
    if (ec || !parser.is_done()) {
        std::cerr
          << (boost::format
              ("WARNING: Incomplete HTTP body in cache (%1% out of %2% bytes) for %3% %4%")
              % body.length() % parser.get()[http::field::content_length] % url % id)
          << std::endl;
        ec = asio::error::invalid_argument;
        return or_throw<std::pair<Response, std::string>>(yield, ec);
    }

    return make_pair(parser.release(), id);
}

} // ouinet::descriptor namespace

} // ouinet namespace
