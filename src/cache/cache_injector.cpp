#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"
#include "http_desc.h"

#include <asio_ipfs.h>
#include "btree_db.h"
#include "publisher.h"
#include "../http_util.h"
#include "../bittorrent/dht.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

CacheInjector::CacheInjector
        ( asio::io_service& ios
        , const boost::optional<util::Ed25519PrivateKey>& bt_publish_key
        , fs::path path_to_repo)
    : _ipfs_node(new asio_ipfs::node(ios, (path_to_repo/"ipfs").native()))
    , _bt_dht(new bt::MainlineDht(ios))
    , _publisher(new Publisher( *_ipfs_node
                              , *_bt_dht
                              , bt_publish_key
                              , path_to_repo/"publisher"))
    , _db(new BTreeInjectorDb(*_ipfs_node, *_publisher, path_to_repo))
    , _scheduler(new Scheduler(ios, _concurrency))
    , _was_destroyed(make_shared<bool>(false))
{
    _bt_dht->set_interfaces({asio::ip::address_v4::any()});
}

string CacheInjector::id() const
{
    return _ipfs_node->id();
}

std::string CacheInjector::put_data( const std::string& data
                                   , boost::asio::yield_context yield)
{
    auto wd = _was_destroyed;
    sys::error_code ec;

    string ipfs_id = _ipfs_node->add(data, yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;

    return or_throw(yield, ec, move(ipfs_id));
}

string CacheInjector::insert_content( Request rq
                                    , Response rs
                                    , asio::yield_context yield)
{
    auto wd = _was_destroyed;

    sys::error_code ec;

    auto id = rs[http_::response_injection_id_hdr].to_string();
    rs.erase(http_::response_injection_id_hdr);

    auto ts = boost::posix_time::microsec_clock::universal_time();

    string desc_data;

    {
        auto slot = _scheduler->wait_for_slot(yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        desc_data = descriptor::http_create(*this, id, ts, rq, rs, yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);
    }

    // TODO: use string_view for key
    auto key = rq.target().to_string();
    _db->update(move(key), desc_data, yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;

    return or_throw(yield, ec, desc_data);
}

string CacheInjector::get_data(const string &ipfs_id, asio::yield_context yield)
{
    return _ipfs_node->cat(ipfs_id, yield);
}

CacheEntry CacheInjector::get_content(string url, asio::yield_context yield)
{
    using std::get;
    sys::error_code ec;

    string desc_data = _db->query(url, yield[ec]);

    if (ec) return or_throw<CacheEntry>(yield, ec);

    // Assemble HTTP response from cached content
    // and attach injection identifier header for injection tracking.
    auto res = descriptor::http_parse(*this, desc_data, yield[ec]);
    get<0>(res).set(http_::response_injection_id_hdr, get<1>(res));
    return or_throw(yield, ec, CacheEntry{get<2>(res), move(get<0>(res))});
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
