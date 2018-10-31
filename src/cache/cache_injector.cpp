#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"
#include "http_desc.h"

#include <asio_ipfs.h>
#include "btree_db.h"
#include "bep44_db.h"
#include "descdb.h"
#include "publisher.h"
#include "../bittorrent/dht.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

CacheInjector::CacheInjector
        ( asio::io_service& ios
        , util::Ed25519PrivateKey bt_privkey
        , fs::path path_to_repo)
    : _ipfs_node(new asio_ipfs::node(ios, (path_to_repo/"ipfs").native()))
    , _bt_dht(new bt::MainlineDht(ios))
    , _publisher(new Publisher(*_ipfs_node, *_bt_dht, bt_privkey))
    , _btree_db(new BTreeInjectorDb(*_ipfs_node, *_publisher, path_to_repo))
    , _scheduler(new Scheduler(ios, _concurrency))
    , _was_destroyed(make_shared<bool>(false))
{
    _bt_dht->set_interfaces({asio::ip::address_v4::any()});
    _bep44_db.reset(new Bep44InjectorDb(*_bt_dht, bt_privkey));
}

string CacheInjector::ipfs_id() const
{
    return _ipfs_node->id();
}

InjectorDb* CacheInjector::get_db(DbType db_type) const
{
    switch (db_type) {
        case DbType::btree: return _btree_db.get();
        case DbType::bep44: return _bep44_db.get();
    }

    return nullptr;
}

string CacheInjector::insert_content( const string& id
                                    , const Request& rq
                                    , const Response& rs
                                    , DbType db_type
                                    , asio::yield_context yield)
{
    auto wd = _was_destroyed;

    // Wraps IPFS add operation to wait for a slot first
    auto ipfs_add = [&](auto data, auto yield) {
        sys::error_code ec;
        auto slot = _scheduler->wait_for_slot(yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        auto cid = _ipfs_node->add(data, yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        return or_throw(yield, ec, move(cid));
    };

    sys::error_code ec;

    // Prepare and create descriptor
    auto ts = boost::posix_time::microsec_clock::universal_time();
    auto desc = descriptor::http_create( id, ts
                                       , rq, rs
                                       , ipfs_add
                                       , yield[ec]);
    if (!ec && *wd) ec = asio::error::operation_aborted;
    if (ec) return or_throw<string>(yield, ec);

    // Store descriptor
    auto db = get_db(db_type);
    descriptor::put_into_db( rq.target().to_string(), desc
                           , *db, ipfs_add, yield[ec]);
    if (!ec && *wd) ec = asio::error::operation_aborted;
    return or_throw(yield, ec, move(desc));
}

string CacheInjector::get_descriptor( string url
                                    , DbType db_type
                                    , Cancel& cancel
                                    , asio::yield_context yield)
{
    auto db = get_db(db_type);

    return descriptor::get_from_db
      ( url, *db
      , [&](auto h, auto& c, auto y) {
            function<void()> cancel_fn;
            auto cancel_handle = c.connect([&] { if (cancel_fn) cancel_fn(); });
            return _ipfs_node->cat(h, cancel_fn, y);
        }
      , cancel, yield);
}

pair<string, CacheEntry>
CacheInjector::get_content( string url
                          , DbType db_type
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_descriptor(url, db_type, cancel, yield[ec]);

    if (ec) return or_throw<pair<string, CacheEntry>>(yield, ec);

    return descriptor::http_parse
      ( desc_data
      , [&](auto h, auto& c, auto y) {
            function<void()> cancel_fn;
            auto cancel_handle = c.connect([&] { if (cancel_fn) cancel_fn(); });
            return _ipfs_node->cat(h, cancel_fn, y);
        }
      , cancel, yield);
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
