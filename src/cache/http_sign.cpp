#include "http_sign.h"

#include <chrono>
#include <ctime>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/format.hpp>

#include "../constants.h"
#include "../util.h"
#include "../util/hash.h"

namespace ouinet { namespace cache {

http::response_header<>
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id)
{
    using namespace ouinet::http_;
    assert(response_version_hdr_current == response_version_hdr_v0);

    rsh.set(response_version_hdr, response_version_hdr_v0);
    rsh.set(header_prefix + "URI", rqh.target());
    {
        auto ts = std::chrono::seconds(std::time(nullptr)).count();
        rsh.set( header_prefix + "Injection"
               , boost::format("id=%s,ts=%d") % injection_id % ts);
    }
    rsh.set(header_prefix + "HTTP-Status", rsh.result_int());

    // Enabling chunking is easier with a whole respone,
    // and we do not care about content length anyway.
    http::response<http::empty_body> rs(std::move(rsh));
    rs.chunked(true);
    auto trfmt = boost::format("%s%s" + header_prefix + "Data-Size, Digest, Signature");
    auto trhdr = rs[http::field::trailer];
    rs.set( http::field::trailer
          , (trfmt % trhdr % (trhdr.empty() ? "" : ", ")).str() );

    return rs.base();
}

http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey& sk
                      , const std::string key_id)
{
    // Pending trailer headers to support the signature.
    rst.set(ouinet::http_::header_prefix + "Data-Size", content_length);
    rst.set(http::field::digest, "SHA-256=" + util::base64_encode(content_digest));

    // Put together the head to be signed:
    // initial head, minus chunking (and related headers), plus trailer headers.
    // Use `...-Data-Size` internal header instead on `Content-Length`.
    http::response<http::empty_body> rs(rsh);
    rs.chunked(false);  // easier with a whole response
    rs.erase(http::field::content_length);  // 0 anyway because of empty body
    rs.erase(http::field::trailer);
    auto to_sign = std::move(rs.base());
    for (auto& hdr : rst)
        to_sign.set(hdr.name_string(), hdr.value());

    rst.set("Signature", http_signature(to_sign, sk, key_id));
    return rst;
}

std::string
http_key_id_for_injection(const ouinet::util::Ed25519PrivateKey& sk)
{
    return "ed25519=" + ouinet::util::base64_encode(sk.public_key().serialize());
}

std::string
http_digest(const http::response<http::dynamic_body>& rs)
{
    ouinet::util::SHA256 hash;

    // Feed each buffer of body data into the hash.
    for (auto it : rs.body().data())
        hash.update(it);
    auto digest = hash.close();
    auto encoded_digest = ouinet::util::base64_encode(digest);
    return "SHA-256=" + encoded_digest;
}

template<class Head>
static void
prep_sig_head(const Head& inh, Head& outh)
{
    using namespace std;

    // Lowercase header names, to more-or-less respect input order.
    vector<string> hdr_sorted;
    // Lowercase header name to `, `-concatenated, trimmed values.
    map<string, string> hdr_values;

    for (auto& hdr : inh) {
        auto name = hdr.name_string().to_string();  // lowercase
        boost::algorithm::to_lower(name);

        auto value_v = hdr.value();  // trimmed
        while (value_v.starts_with(' ')) value_v.remove_prefix(1);
        while (value_v.ends_with  (' ')) value_v.remove_suffix(1);

        auto vit = hdr_values.find(name);
        if (vit == hdr_values.end()) {  // new entry, add
            hdr_values[name] = value_v.to_string();
            hdr_sorted.push_back(name);
        } else {  // existing entry, concatenate
            vit->second += ", ";
            vit->second.append(value_v.data(), value_v.length());
        }
    }

    for (auto name : hdr_sorted)
        outh.set(name, hdr_values[name]);
}

template<class Head>
static std::pair<std::string, std::string>
get_sig_str_hdrs(const Head& sig_head)
{
    std::string sig_string, headers;
    bool ins_sep = false;
    for (auto& hdr : sig_head) {
        auto name = hdr.name_string();
        auto value = hdr.value();

        if (ins_sep) sig_string += '\n';
        sig_string += (boost::format("%s: %s") % name % value).str();

        if (ins_sep) headers += ' ';
        headers.append(name.data(), name.length());

        ins_sep = true;
    }

    return {sig_string, headers};
}

std::string
http_signature( const http::response_header<>& rsh
              , const ouinet::util::Ed25519PrivateKey& sk
              , const std::string key_id)
{
    auto fmt = boost::format("keyId=\"%s\""
                             ",algorithm=\"hs2019\""
                             ",created=%d"
                             ",headers=\"%s\""
                             ",signature=\"%s\"");

    auto ts = std::chrono::seconds(std::time(nullptr)).count();

    http::response_header<> sig_head;
    sig_head.set("(created)", ts);
    prep_sig_head(rsh, sig_head);  // unique fields, lowercase names, trimmed values

    std::string sig_string, headers;
    std::tie(sig_string, headers) = get_sig_str_hdrs(sig_head);

    auto encoded_sig = ouinet::util::base64_encode(sk.sign(sig_string));

    return (fmt % key_id % ts % headers % encoded_sig).str();
}

}} // namespaces
