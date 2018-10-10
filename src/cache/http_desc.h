// Temporary, simplified URI descriptor format for a single HTTP response.
//
// See `doc/descriptor-*.json` for the target format.
#pragma once

#include <sstream>

#include <boost/format.hpp>
#include <boost/optional.hpp>
#include <json.hpp>
#include <asio_ipfs.h>

#include "cache_entry.h"
#include "../namespaces.h"
#include "../or_throw.h"
#include "../http_util.h"

namespace ouinet {

struct Descriptor {
    using ptime = boost::posix_time::ptime;

    static unsigned version() { return 0; }

    std::string url;
    std::string request_id;
    ptime       timestamp;
    std::string head;
    std::string body_link;

    std::string serialize() const {
        static const auto ts_to_str = [](ptime ts) {
            return boost::posix_time::to_iso_extended_string(ts) + 'Z';
        };

        return nlohmann::json { { "version"   , version() }
                              , { "url"       , url }
                              , { "id"        , request_id }
                              , { "ts"        , ts_to_str(timestamp) }
                              , { "head"      , head }
                              , { "body_link" , body_link }
                              }
                              .dump();
    }

    static boost::optional<Descriptor> deserialize(std::string data) {
        try {
            auto json = nlohmann::json::parse(data);

            auto v = json["version"];

            if (!v.is_null() && unsigned(v) != version()) {
                return boost::none;
            }

            Descriptor dsc;

            dsc.url        = json["url"];
            dsc.request_id = json["id"];
            dsc.timestamp  = boost::posix_time::from_iso_extended_string(json["ts"]);
            dsc.head       = json["head"];
            dsc.body_link  = json["body_link"];

            return dsc;
        } catch (const std::exception& e) {
            return boost::none;
        }
    }
};

namespace descriptor {

// For the given HTTP request `rq` and response `rs`, seed body data to the `cache`,
// then create an HTTP descriptor with the given `id` for the URL and response,
// and return it.
static inline
std::pair<std::string /* ipfs */, std::string /* body */>
http_create( asio_ipfs::node& ipfs
           , const std::string& id
           , boost::posix_time::ptime ts
           , const http::request<http::string_body>& rq
           , const http::response<http::dynamic_body>& rs
           , asio::yield_context yield) {

    using namespace std;
    using Ret = pair<string, string>;

    sys::error_code ec;

    string ipfs_id = ipfs.add(
            beast::buffers_to_string(rs.body().data()), yield[ec]);

    auto url = rq.target();

    if (ec) return or_throw<Ret>(yield, ec);

    auto rs_ = rs;

    rs_.erase(http::field::transfer_encoding);

    stringstream rsh_ss;
    rsh_ss << rs_.base();

    string descriptor = Descriptor{ url.to_string()
                                  , id
                                  , ts
                                  , rsh_ss.str()
                                  , ipfs_id
                                  }.serialize();

    string cid = ipfs.add(descriptor, yield[ec]);

    return or_throw<Ret>(yield, ec, { move(cid), move(descriptor) });
}

// For the given HTTP descriptor serialized in `desc_data`,
// retrieve the head from the descriptor and the body data from the `cache`,
// assemble and return the HTTP response along with its identifier.
static inline
CacheEntry http_parse( asio_ipfs::node& ipfs
                     , const std::string& desc_ipfs
                     , asio::yield_context yield)
{

    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;

    std::string desc_data = ipfs.cat(desc_ipfs, yield[ec]);

    if (ec) return or_throw<CacheEntry>(yield, ec);

    boost::optional<Descriptor> dsc = Descriptor::deserialize(desc_data);

    if (!dsc) {
        std::cerr << "WARNING: Malformed or invalid HTTP descriptor: " << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << desc_data << std::endl;
        std::cerr << "----------------" << std::endl;
        ec = asio::error::invalid_argument;
    }

    if (ec) return or_throw<CacheEntry>(yield, ec);

    // Get the HTTP response body (stored independently).
    std::string body = ipfs.cat(dsc->body_link, yield[ec]);

    // Build an HTTP response from the head in the descriptor and the retrieved body.
    http::response_parser<Response::body_type> parser;
    parser.eager(true);

    // - Parse the response head.
    parser.put(asio::buffer(dsc->head), ec);

    if (!ec && !parser.is_header_done()) {
        ec = asio::error::invalid_argument;
    }

    if (ec) {
        std::cerr << "WARNING: Malformed or incomplete HTTP head in descriptor" << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << dsc->head << std::endl;
        std::cerr << "----------------" << std::endl;
        return or_throw<CacheEntry>(yield, ec);
    }

    Response res = parser.release();
    Response::body_type::reader reader(res, res.body());
    reader.put(asio::buffer(body), ec);

    if (ec) {
        std::cerr << "WARNING: Failed to put body into the response "
            << ec.message() << std::endl;

        return or_throw<CacheEntry>(yield, asio::error::invalid_argument);
    }

    res.set(http_::response_injection_id_hdr, dsc->request_id);

    res.prepare_payload();

    return CacheEntry{dsc->timestamp, std::move(res)};
}

} // ouinet::descriptor namespace

} // ouinet namespace
