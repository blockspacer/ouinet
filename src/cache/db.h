#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <string>
#include <json.hpp>

#include "../namespaces.h"
#include "../util/condition_variable.h"
#include "resolver.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

class BTree;
class Publisher;
using Json = nlohmann::json;

class ClientDb {
    using OnDbUpdate = std::function<void(const sys::error_code&)>;

public:
    ClientDb( asio_ipfs::node&
            , std::string ipns
            , bittorrent::MainlineDht& bt_dht
            , boost::optional<util::Ed25519PublicKey> bt_publish_pubkey
            , fs::path path_to_repo);

    std::string query(std::string key, asio::yield_context);

    boost::asio::io_service& get_io_service();

    const std::string& ipns() const { return _ipns; }
    const std::string& ipfs() const { return _ipfs; }

    asio_ipfs::node& ipfs_node() { return _ipfs_node; }

    const BTree* get_btree() const;

    ~ClientDb();

private:
    void on_resolve(std::string cid, asio::yield_context);

private:
    const fs::path _path_to_repo;
    std::string _ipns;
    std::string _ipfs; // Last known
    asio_ipfs::node& _ipfs_node;
    std::unique_ptr<BTree> _db_map;
    Resolver _resolver;
    std::shared_ptr<bool> _was_destroyed;
};

class InjectorDb {
public:
    InjectorDb(asio_ipfs::node&, Publisher&, fs::path path_to_repo);

    std::string query(std::string key, asio::yield_context);

    boost::asio::io_service& get_io_service();

    const std::string& ipns() const { return _ipns; }

    asio_ipfs::node& ipfs_node() { return _ipfs_node; }

    void update(std::string key, std::string value, asio::yield_context);

    ~InjectorDb();

private:
    void publish(std::string);
    void continuously_upload_db(asio::yield_context);

private:
    const fs::path _path_to_repo;
    std::string _ipns;
    asio_ipfs::node& _ipfs_node;
    Publisher& _publisher;
    std::unique_ptr<BTree> _db_map;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

