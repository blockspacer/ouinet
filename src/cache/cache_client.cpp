#include <asio_ipfs.h>
#include "cache_client.h"
#include "btree_db.h"
#include "bep44_db.h"
#include "cache_entry.h"
#include "descdb.h"
#include "http_desc.h"
#include "ipfs_util.h"
#include "../or_throw.h"
#include "../bittorrent/dht.h"
#include "../util/crypto.h"

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
                  , function<void()>& cancel
                  , asio::yield_context yield)
{
    using ClientP = unique_ptr<CacheClient>;

    bool canceled = false;

    cancel = [&canceled] {
        cout << "TODO: Canceling Client::build doesn't immediately stop "
             << "IO tasks" << endl;;

        canceled = true;
    };

    sys::error_code ec;
    auto ipfs_node = asio_ipfs::node::build( ios
                                           , (path_to_repo/"ipfs").native()
                                           , yield[ec]);

    cancel = nullptr;

    if (canceled) {
        ec = asio::error::operation_aborted;
    }

    if (ec) return or_throw<ClientP>(yield, ec);

    return ClientP(new CacheClient( move(*ipfs_node)
                                  , move(ipns)
                                  , std::move(bt_pubkey)
                                  , move(path_to_repo)));
}

CacheClient::CacheClient( asio_ipfs::node ipfs_node
                        , string ipns
                        , optional<util::Ed25519PublicKey> bt_pubkey
                        , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(new asio_ipfs::node(move(ipfs_node)))
    , _bt_dht(new bt::MainlineDht(_ipfs_node->get_io_service()))
    , _btree_db(new BTreeClientDb( *_ipfs_node
                                 , ipns
                                 , *_bt_dht
                                 , bt_pubkey
                                 , _path_to_repo))
{
    _bt_dht->set_interfaces({asio::ip::address_v4::any()});

    if (bt_pubkey) {
        _bep44_db.reset(new Bep44ClientDb(*_bt_dht, *bt_pubkey));
    }
}

const BTree* CacheClient::get_btree() const
{
    if (!_ipfs_node) return nullptr;
    if (!_btree_db) return nullptr;
    return _btree_db->get_btree();
}

string CacheClient::ipfs_add(const string& data, asio::yield_context yield)
{
    return _ipfs_node->add(data, yield);
}

ClientDb* CacheClient::get_db(DbType db_type)
{
    switch (db_type) {
        case DbType::btree: return _btree_db.get();
        case DbType::bep44: return _bep44_db.get();
    }

    assert(0);
    return nullptr;
}

string CacheClient::get_descriptor( string url
                                  , DbType db_type
                                  , Cancel& cancel
                                  , asio::yield_context yield)
{
    auto db = get_db(db_type);

    if (!db) return or_throw<string>(yield, asio::error::not_found);

    return descriptor::get_from_db
        ( url, *db, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

pair<string, CacheEntry>
CacheClient::get_content( string url
                        , DbType db_type
                        , Cancel& cancel
                        , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_descriptor(url, db_type, cancel, yield[ec]);

    if (ec) return or_throw<pair<string, CacheEntry>>(yield, ec);

    return descriptor::http_parse
        ( desc_data, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

void CacheClient::set_ipns(std::string ipns)
{
    assert(0 && "TODO");
    //_btree_db.reset(new ClientDb(*_ipfs_node, _path_to_repo, move(ipns)));
}

std::string CacheClient::ipfs_id() const
{
    return _ipfs_node->id();
}

string CacheClient::ipns() const
{
    if (!_btree_db) return {};
    return _btree_db->ipns();
}

string CacheClient::ipfs() const
{
    if (!_btree_db) return {};
    return _btree_db->ipfs();
}

CacheClient::~CacheClient() {}
