#include <asio_ipfs.h>
#include "cache_client.h"
#include "btree_index.h"
#include "bep44_index.h"
#include "cache_entry.h"
#include "descidx.h"
#include "http_desc.h"
#include "ipfs_util.h"
#include "../or_throw.h"
#include "../async_sleep.h"
#include "../bittorrent/dht.h"
#include "../logger.h"
#include "../util/crypto.h"
#include "../util/watch_dog.h"

using namespace std;
using namespace ouinet;

namespace asio = boost::asio;
namespace sys  = boost::system;
namespace bt   = ouinet::bittorrent;

using boost::optional;

unique_ptr<CacheClient>
CacheClient::build( asio::io_service& ios
                  , string ipns
                  , optional<util::Ed25519PublicKey> bt_pubkey
                  , fs::path path_to_repo
                  , bool autoseed_updated
                  , unsigned int bep44_index_capacity
                  , Cancel& cancel
                  , asio::yield_context yield)
{
    using ClientP = unique_ptr<CacheClient>;

    sys::error_code ec;

    unique_ptr<asio_ipfs::node> ipfs_node;

    {
        auto slot = cancel.connect([&] {
            cerr << "TODO: Canceling CacheClient::build doesn't immediately stop "
                 << "IO tasks\n";
        });

        asio_ipfs::node::config cfg {
            .online       = true,
            // The default values 600/900/20 kill routers
            // See the Swarm section here for more info:
            // https://medium.com/textileio/tutorial-setting-up-an-ipfs-peer-part-iii-f5f43506874c
            .low_water    = 20,
            .high_water   = 50,
            .grace_period = 120
        };

        ipfs_node = asio_ipfs::node::build( ios
                                          , (path_to_repo/"ipfs").native()
                                          , cfg
                                          , yield[ec]);
    }

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<ClientP>(yield, ec);

    auto bt_dht = make_unique<bt::MainlineDht>(ios);

    bt_dht->set_interfaces({asio::ip::address_v4::any()});

    unique_ptr<Bep44ClientIndex> bep44_index;

    if (bt_pubkey) {
        bep44_index = Bep44ClientIndex::build(*bt_dht, *bt_pubkey
                                             , path_to_repo / "bep44-index"
                                             , bep44_index_capacity
                                             , cancel
                                             , yield[ec]);

        assert(!cancel || ec == asio::error::operation_aborted);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<ClientP>(yield, ec);
    }

    return ClientP(new CacheClient( move(ipfs_node)
                                  , move(ipns)
                                  , std::move(bt_pubkey)
                                  , std::move(bt_dht)
                                  , std::move(bep44_index)
                                  , move(path_to_repo)
                                  , autoseed_updated));
}

// private
CacheClient::CacheClient( std::unique_ptr<asio_ipfs::node> ipfs_node
                        , string ipns
                        , optional<util::Ed25519PublicKey> bt_pubkey
                        , unique_ptr<bittorrent::MainlineDht> bt_dht
                        , unique_ptr<Bep44ClientIndex> bep44_index
                        , fs::path path_to_repo
                        , bool autoseed_updated)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(move(ipfs_node))
    , _bt_dht(move(bt_dht))
    , _bep44_index(move(bep44_index))
{
    ClientIndex::UpdatedHook updated_hook([&, autoseed_updated]
                                          (auto o, auto n, auto& c, auto y) noexcept
    {
        // Returning false in this function avoids the republication of index entries
        // whose linked descriptors are missing or malformed,
        // or whose associated data cannot be retrieved.

        auto ipfs_load = IPFS_LOAD_FUNC(*_ipfs_node);
        sys::error_code ec;
        Cancel cancel(c);
        WatchDog wd( _ipfs_node->get_io_service()
                   // Even when not reseeding data, allow some time to reseed linked descriptor.
                   , autoseed_updated ? chrono::minutes(3) : chrono::seconds(30)  // TODO: adjust
                   , [&] { cancel(); });

        // Fetch and decode new descriptor.
        auto desc_data = descriptor::from_path(n, ipfs_load, cancel, y[ec]);
        if (ec || cancel) return false;
        auto desc = Descriptor::deserialize(desc_data);
        if (!desc) return false;

        if (!autoseed_updated) return true;  // do not care about data

        // Fetch data pointed by new descriptor.
        // TODO: check if it matches that of old descriptor
        auto data = ipfs_load(desc->body_link, cancel, y[ec]);
        if (cancel && !c) ec = asio::error::timed_out;

        LOG_DEBUG( "Fetch data from updated index entry:"
                 , " ec=\"", ec.message(), "\""
                 , " ipfs_cid=", desc->body_link," url=", desc->url);
        return !ec;
    });

    if (!ipns.empty()) {
        _btree_index.reset(new BTreeClientIndex( *_ipfs_node
                                               , ipns
                                               , *_bt_dht
                                               , bt_pubkey
                                               , _path_to_repo));
    }

    // Setup hooks.
    // Since indexes may start working right after construction,
    // setting hooks like this leaves a gap for
    // some updates to be detected by an index before the hook is set.
    // It is done like this to be able to create indexes in `build`
    // while retaining ownership of the IPFS node object here.
    _bep44_index->updated_hook(move(updated_hook));
}

const BTree* CacheClient::get_btree() const
{
    if (!_ipfs_node) return nullptr;
    if (!_btree_index) return nullptr;
    return _btree_index->get_btree();
}

string CacheClient::ipfs_add(const string& data, asio::yield_context yield)
{
    if (!_ipfs_node) return or_throw<string>(yield, asio::error::operation_not_supported);
    return _ipfs_node->add(data, yield);
}

string CacheClient::insert_mapping( const boost::string_view target
                                  , const std::string& ins_data
                                  , IndexType index_type
                                  , Cancel& cancel
                                  , boost::asio::yield_context yield)
{
    auto index = get_index(index_type);

    if (!index) return or_throw<string>( yield
                                       , asio::error::operation_not_supported);

    return index->insert_mapping(target, ins_data, cancel, yield);
}

ClientIndex* CacheClient::get_index(IndexType index_type)
{
    switch (index_type) {
        case IndexType::btree: return _btree_index.get();
        case IndexType::bep44: return _bep44_index.get();
    }

    assert(0);
    return nullptr;
}

string CacheClient::get_descriptor( const string& key
                                  , IndexType index_type
                                  , Cancel& cancel
                                  , asio::yield_context yield)
{
    auto index = get_index(index_type);

    if (!index) return or_throw<string>(yield, asio::error::not_found);

    sys::error_code ec;

    string desc_path = index->find(key, cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    return descriptor_from_path(desc_path, cancel, yield);
}

string CacheClient::descriptor_from_path( const string& desc_path
                                        , Cancel& cancel
                                        , asio::yield_context yield)
{
    if (!_ipfs_node)
        return or_throw<string>(yield, asio::error::operation_not_supported);

    return descriptor::from_path
        ( desc_path, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

pair<string, CacheEntry>
CacheClient::get_content( const string& key
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

void CacheClient::set_ipns(std::string ipns)
{
    assert(0 && "TODO");
    //_btree_index.reset(new ClientIndex(*_ipfs_node, _path_to_repo, move(ipns)));
}

std::string CacheClient::ipfs_id() const
{
    assert(_ipfs_node);
    return _ipfs_node->id();
}

string CacheClient::ipns() const
{
    if (!_btree_index) return {};
    return _btree_index->ipns();
}

string CacheClient::ipfs() const
{
    if (!_btree_index) return {};
    return _btree_index->ipfs();
}

void
CacheClient::wait_for_ready(Cancel& cancel, asio::yield_context yield) const
{
    // TODO: Wait for IPFS cache to be ready, if needed.
    LOG_DEBUG("BEP44 index: waiting for BitTorrent DHT bootstrap...");
    _bt_dht->wait_all_ready(yield, cancel);
    LOG_DEBUG("BEP44 index: bootstrapped BitTorrent DHT");  // used by integration tests
}

CacheClient::~CacheClient() {}
