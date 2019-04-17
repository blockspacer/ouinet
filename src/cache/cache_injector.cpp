#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"
#include "http_desc.h"

#include <asio_ipfs.h>
#include "btree_index.h"
#include "bep44_index.h"
#include "descidx.h"
#include "publisher.h"
#include "ipfs_util.h"
#include "../async_sleep.h"
#include "../bittorrent/dht.h"
#include "../logger.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

// static
unique_ptr<CacheInjector> CacheInjector::build( boost::asio::io_service& ios
                                              , util::Ed25519PrivateKey bt_privkey
                                              , fs::path path_to_repo
                                              , bool enable_btree
                                              , bool enable_bep44
                                              , unsigned int bep44_index_capacity
                                              , Cancel& cancel
                                              , boost::asio::yield_context yield)
{
    using Ret = unique_ptr<CacheInjector>;

    sys::error_code ec;

    // TODO: Make this a parameter
    const bool online = false;

    unique_ptr<bt::MainlineDht> bt_dht(new bt::MainlineDht(ios));

    if (online) {
        bt_dht->set_interfaces({asio::ip::address_v4::any()});
    }

    unique_ptr<Bep44InjectorIndex> bep44_index;

    if (enable_bep44) {
        bep44_index = Bep44InjectorIndex::build(*bt_dht
                                               , bt_privkey
                                               , path_to_repo / "bep44-index"
                                               , bep44_index_capacity
                                               , cancel
                                               , yield[ec]);
    }

    assert(!cancel || ec == asio::error::operation_aborted);
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<Ret>(yield, ec);

    Ret ci(new CacheInjector( ios
                            , bt_privkey
                            , path_to_repo
                            , enable_btree
                            , move(bt_dht)
                            , move(bep44_index)));

    if (online) {
        ci->wait_for_ready(cancel, yield[ec]);
    }

    assert(!cancel || ec == asio::error::operation_aborted);
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<Ret>(yield, ec);

    return or_throw(yield, ec, move(ci));
}


CacheInjector::CacheInjector
        ( asio::io_service& ios
        , util::Ed25519PrivateKey bt_privkey
        , fs::path path_to_repo
        , bool enable_btree
        , unique_ptr<bt::MainlineDht> bt_dht
        , unique_ptr<Bep44InjectorIndex> bep44_index)
    : _ipfs_node(new asio_ipfs::node(ios, false, (path_to_repo/"ipfs").native()))
    , _bt_dht(move(bt_dht))  // used by either B-tree over BEP44, or BEP44
    , _bep44_index(move(bep44_index))
    , _scheduler(new Scheduler(ios, _concurrency))
{
    assert((enable_btree || _bep44_index) && "At least one index type must be enabled");

    if (enable_btree) {
        _publisher.reset(new Publisher(*_ipfs_node, *_bt_dht, bt_privkey));
        _btree_index.reset(new BTreeInjectorIndex(*_ipfs_node, *_publisher, path_to_repo));
    }
}

string CacheInjector::ipfs_id() const
{
    return _ipfs_node->id();
}

InjectorIndex* CacheInjector::get_index(IndexType index_type) const
{
    switch (index_type) {
        case IndexType::btree: return _btree_index.get();
        case IndexType::bep44: return _bep44_index.get();
    }

    return nullptr;
}

CacheInjector::InsertionResult
CacheInjector::insert_content( const string& id
                             , const Request& rq
                             , Response rs
                             , IndexType index_type
                             , bool perform_io
                             , asio::yield_context yield)
{
    Cancel cancel(_cancel);

    auto index = get_index(index_type);

    if (!index)
        return or_throw<InsertionResult>( yield
                                        , asio::error::operation_not_supported);

    // Wraps IPFS add operation to wait for a slot first
    auto ipfs_add = [&](auto data, auto yield) {
        sys::error_code ec;
        Scheduler::Slot slot;

        if (perform_io) {
            slot = _scheduler->wait_for_slot(yield[ec]);
        }

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        string cid;

        if (perform_io) {
            cid = _ipfs_node->add(data, yield[ec]);
        } else {
            function<void()> cancel_fn;
            auto slot = cancel.connect([&] { if (cancel_fn) cancel_fn(); });
            cid = _ipfs_node->calculate_cid(data, cancel_fn, yield[ec]);
        }

        if (cancel) ec = asio::error::operation_aborted;
        return or_throw(yield, ec, move(cid));
    };

    sys::error_code ec;

    // Prepare and create descriptor
    auto ts = boost::posix_time::microsec_clock::universal_time();
    auto desc = descriptor::http_create( id, ts
                                       , rq, rs
                                       , ipfs_add
                                       , yield[ec]);

    rs = Response(); // Free the memory

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<InsertionResult>(yield, ec);

    // Store descriptor
    auto key = key_from_http_req(rq);
    auto cid_insdata = descriptor::put_into_index
        (key, desc, *index, ipfs_add, perform_io, yield[ec]);

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<InsertionResult>(yield, ec);

    return InsertionResult { move(key)
                           , move(desc)
                           , "/ipfs/" + cid_insdata.first
                           , move(cid_insdata.second)};
}

std::string CacheInjector::ipfs_cat( boost::string_view cid
                                   , Cancel& cancel
                                   , boost::asio::yield_context yield)
{
    if (!_ipfs_node) {
        return or_throw<string>(yield, asio::error::operation_not_supported);
    }

    return ::ouinet::ipfs_cat(*_ipfs_node, cid, cancel, yield);
}

string CacheInjector::get_bep44m( boost::string_view key
                                , Cancel& cancel
                                , boost::asio::yield_context yield)
{
    auto index = get_index(IndexType::bep44);

    if (!index)
        return or_throw<string>(yield, asio::error::operation_not_supported);

    // TODO: Modify find to accept string_view
    return index->find(key.to_string(), cancel, yield);
}

string CacheInjector::get_descriptor( const string& key
                                    , IndexType index_type
                                    , Cancel& cancel
                                    , asio::yield_context yield)
{
    auto index = get_index(index_type);

    if (!index)
        return or_throw<string>(yield, asio::error::operation_not_supported);

    sys::error_code ec;

    string desc_path = index->find(key, cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    return descriptor::from_path
        ( desc_path, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

Descriptor CacheInjector::bep44m_to_descriptor( boost::string_view bep44m_s
                                              , Cancel& cancel
                                              , asio::yield_context yield)
{
    auto opt_bep44m = bittorrent::MutableDataItem::bdecode(bep44m_s);

    if (!opt_bep44m) {
        return or_throw<Descriptor>(yield, asio::error::invalid_argument);
    }

    auto opt_path = opt_bep44m->value.as_string();

    if (!opt_path) {
        return or_throw<Descriptor>(yield, asio::error::invalid_argument);
    }

    sys::error_code ec;

    string desc_str = descriptor::from_path( *opt_path
                                           , IPFS_LOAD_FUNC(*_ipfs_node)
                                           , cancel
                                           , yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, Descriptor());

    auto opt_desc = Descriptor::deserialize(desc_str);

    if (!opt_desc) {
        return or_throw<Descriptor>(yield, asio::error::bad_descriptor);
    }

    return move(*opt_desc);
}

pair<string, CacheEntry>
CacheInjector::get_content( const string& key
                          , IndexType index_type
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_descriptor(key, index_type, cancel, yield[ec]);

    if (ec) return or_throw<pair<string, CacheEntry>>(yield, ec);

    return descriptor::http_parse
        ( desc_data, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

void
CacheInjector::wait_for_ready(Cancel& cancel, asio::yield_context yield) const
{
    // TODO: Wait for IPFS cache to be ready, if needed.
    if (_bep44_index) {
        LOG_DEBUG("BEP44 index: waiting for BitTorrent DHT bootstrap...");
        _bt_dht->wait_all_ready(yield, cancel);
        LOG_DEBUG("BEP44 index: bootstrapped BitTorrent DHT");  // used by integration tests
    }
}

CacheInjector::~CacheInjector()
{
    _cancel();
}
