#pragma once

#include "../../logger.h"
#include "../../util/crypto.h"
#include "../../util/yield.h"
#include "../cache_entry.h"
#include <boost/filesystem.hpp>

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
}

class Session;

namespace cache {
namespace bep5_http {

class Client {
private:
    struct Impl;

public:
    static std::unique_ptr<Client>
    build( std::shared_ptr<bittorrent::MainlineDht>
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , log_level_t
         , asio::yield_context);

    Session load(const std::string& key, Cancel, Yield);

    void store( const std::string& key
              , Session&
              , Cancel
              , asio::yield_context);

    // Get the newest protocol version that has been seen in the network
    // (e.g. to warn about potential upgrades).
    unsigned get_newest_proto_version() const;

    ~Client();

    void        set_log_level(log_level_t);
    log_level_t get_log_level() const;

private:
    Client(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}}} // namespaces
