// Utility functions to handle storing descriptors in data bases.

#pragma once

#include <iostream>
#include <string>

#include "../namespaces.h"
#include "../or_throw.h"
#include "../util.h"
#include "db.h"
#include "bep44_db.h"


namespace ouinet {

namespace descriptor {

static const std::string ipfs_prefix = "/ipfs/";
static const std::string zlib_prefix = "/zlib/";

// This is a decision we take here and not at the db level,
// since a db just stores a string
// and it does not differentiate between an inlined descriptor and a link to it.
// An alternative would be to always attempt to store the descriptor inlined
// and attempt again with a link in case of getting `asio::error::message_size`.
// However at the moment we do not want to even attempt inlining
// with the IPFS-based B-tree cache index.
inline
bool db_can_inline(InjectorDb& db) {
    return false;
}

inline
bool db_can_inline(Bep44InjectorDb& db) {
    return true;  // only attempt inlining with BEP44
}

// Get the serialized descriptor pointed to by an entry
// in the given `db` under the given `key`.
// The descriptor has been saved in the given stores (`ipfs_load`).
template <class LoadFunc>
inline
std::string get_from_db( const std::string& key
                       , ClientDb& db
                       , LoadFunc ipfs_load
                       , Cancel& cancel
                       , asio::yield_context yield)
{
    using namespace std;

    sys::error_code ec;

    string desc_data = db.find(key, cancel, yield[ec]);

    if (ec)
        return or_throw<string>(yield, ec);

    string desc_str;
    if (desc_data.find(zlib_prefix) == 0) {
        // Retrieve descriptor from inline zlib-compressed data.
        string desc_zlib(move(desc_data.substr(zlib_prefix.length())));
        desc_str = util::zlib_decompress(desc_zlib, ec);
    } else if (desc_data.find(ipfs_prefix) == 0) {
        // Retrieve descriptor from IPFS link.
        string desc_ipfs(move(desc_data.substr(ipfs_prefix.length())));
        desc_str = ipfs_load(desc_ipfs, cancel, yield[ec]);
    } else {
        cerr << "WARNING: Invalid index entry for descriptor of key: " << key << endl;
        ec = asio::error::not_found;
    }

    return or_throw(yield, ec, move(desc_str));
}

// Add an entry for the serialized descriptor `desc_data`
// in the given `db` under the given `key`.
// The descriptor is to be saved in the given stores (`ipfs_store`).
//
// Returns the result of `ipfs_store` and
// db-specific data to help reinsert the key->descriptor mapping.
template <class StoreFunc>
inline
std::pair<std::string, std::string>
put_into_db( const std::string& key, const std::string& desc_data
           , InjectorDb& db
           , StoreFunc ipfs_store
           , asio::yield_context yield)
{
    using namespace std;
    sys::error_code ec;

    // Always store the descriptor itself in IPFS.
    string desc_ipfs = ipfs_store(desc_data, yield[ec]);

    string ins_data;
    // Insert descriptor inline (if possible).
    bool can_inline = db_can_inline(db);
    if (!ec && can_inline) {
        auto compressed_desc = util::zlib_compress(desc_data);
        ins_data = db.insert(key, zlib_prefix + compressed_desc, yield[ec]);
    }
    // Insert IPFS link to descriptor.
    if (!can_inline || ec == asio::error::message_size) {
        ins_data = db.insert(key, ipfs_prefix + desc_ipfs, yield[ec]);
    }

    pair<string, string> ret(move(desc_ipfs), move(ins_data));
    return or_throw(yield, ec, move(ret));
}

} // namespace descriptor
} // namespace ouinet
