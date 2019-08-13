#include "http_sign.h"

#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/format.hpp>

#include "../constants.h"
#include "../logger.h"
#include "../split_string.h"
#include "../util.h"
#include "../util/hash.h"

namespace ouinet { namespace cache {

static const auto initial_signature_hdr = http_::response_signature_hdr_pfx + "0";
static const auto final_signature_hdr = http_::response_signature_hdr_pfx + "1";

// The only signature algorithm supported by this implementation.
static const std::string sig_alg_hs2019("hs2019");

static
http::response_header<>
without_framing(const http::response_header<>& rsh)
{
    http::response<http::empty_body> rs(rsh);
    rs.chunked(false);  // easier with a whole response
    rs.erase(http::field::content_length);  // 0 anyway because of empty body
    rs.erase(http::field::trailer);
    return rs.base();
}

http::response_header<>
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , std::chrono::seconds::rep injection_ts
                   , const ouinet::util::Ed25519PrivateKey& sk
                   , const std::string& key_id)
{
    using namespace ouinet::http_;
    assert(response_version_hdr_current == response_version_hdr_v0);

    rsh.set(response_version_hdr, response_version_hdr_v0);
    rsh.set(header_prefix + "URI", rqh.target());
    rsh.set( header_prefix + "Injection"
           , boost::format("id=%s,ts=%d") % injection_id % injection_ts);

    // Create a signature of the initial head.
    auto to_sign = without_framing(rsh);
    rsh.set(initial_signature_hdr, http_signature(to_sign, sk, key_id, injection_ts));

    // Enabling chunking is easier with a whole respone,
    // and we do not care about content length anyway.
    http::response<http::empty_body> rs(std::move(rsh));
    rs.chunked(true);
    static const std::string trfmt_ = ( "%s%s"
                                      + header_prefix + "Data-Size, Digest, "
                                      + final_signature_hdr);
    auto trfmt = boost::format(trfmt_);
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
                      , const std::string& key_id
                      , std::chrono::seconds::rep ts)
{
    using namespace ouinet::http_;
    // Pending trailer headers to support the signature.
    rst.set(header_prefix + "Data-Size", content_length);
    rst.set(http::field::digest, "SHA-256=" + util::base64_encode(content_digest));

    // Put together the head to be signed:
    // initial head, minus chunking (and related headers) and its signature,
    // plus trailer headers.
    // Use `...-Data-Size` internal header instead on `Content-Length`.
    auto to_sign = without_framing(rsh);
    to_sign.erase(initial_signature_hdr);
    for (auto& hdr : rst)
        to_sign.set(hdr.name_string(), hdr.value());

    rst.set(final_signature_hdr, http_signature(to_sign, sk, key_id, ts));
    return rst;
}

struct HttpSignature {
    // TODO: refine types
    boost::string_view keyId;
    boost::string_view algorithm;
    boost::string_view created;
    boost::string_view expires;
    boost::string_view headers;
    boost::string_view signature;

    static
    boost::optional<HttpSignature> parse(boost::string_view sig)
    {
        // TODO: proper support for quoted strings
        if (has_comma_in_quotes(sig)) {
            LOG_WARN("Commas in quoted arguments of HTTP signatures are not yet supported");
            return {};
        }

        HttpSignature hs;
        static const std::string def_headers = "(created)";
        hs.headers = def_headers;  // missing is not the same as empty

        for (boost::string_view item : SplitString(sig, ',')) {
            beast::string_view key, value;
            std::tie(key, value) = split_string_pair(item, '=');
            // Unquoted values:
            if (key == "created") {hs.created = value; continue;}
            if (key == "expires") {hs.expires = value; continue;}
            // Quoted values:
            if (value.size() < 2 || value[0] != '"' || value[value.size() - 1] != '"')
                return {};
            value.remove_prefix(1);
            value.remove_suffix(1);
            if (key == "keyId") {hs.keyId = value; continue;}
            if (key == "algorithm") {hs.algorithm = value; continue;}
            if (key == "headers") {hs.headers = value; continue;}
            if (key == "signature") {hs.signature = value; continue;}
            return {};
        }
        if (hs.keyId.empty() || hs.signature.empty()) {  // required
            LOG_WARN("HTTP signature contains empty key identifier or signature");
            return {};
        }
        if (hs.algorithm.empty() || hs.created.empty() || hs.headers.empty()) {  // recommended
            LOG_WARN("HTTP signature contains empty algorithm, creation time stamp, or header list");
        }

        return {std::move(hs)};
    }

    bool verify( const http::response_header<>& rsh
               , const ouinet::util::Ed25519PublicKey& pk)
    {
        // The key may imply an algorithm,
        // but an explicit algorithm should not conflict with the key.
        assert(algorithm.empty() || algorithm == sig_alg_hs2019);
        return true;  // TODO: implement
    }

private:
    static inline
    bool has_comma_in_quotes(const boost::string_view& s) {
        // A comma is between quotes if
        // the number of quotes before it is odd.
        int quotes_seen = 0;
        for (auto c : s) {
            if (c == '"') {
                quotes_seen++;
                continue;
            }
            if ((c == ',') && (quotes_seen % 2 != 0))
                return true;
        }
        return false;
    }
};

bool
http_injection_verify( const http::response_header<>& rsh
                     , const ouinet::util::Ed25519PublicKey& pk
                     , sys::error_code& ec)
{
    auto keyId = http_key_id_for_injection(pk);  // TODO: cache this
    bool sig_found = false;

    for (auto& hdr : rsh) {
        auto hn = hdr.name_string();
        if (!boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx))
            continue;  // not a signature header
        auto sig = HttpSignature::parse(hdr.value());
        if (!sig) {
            LOG_WARN("Invalid HTTP signature in header: ", hn);
            continue;  // not with a usable value
        }
        if (sig->keyId != keyId)
            continue;  // not for the public key we specified
        if (!(sig->algorithm.empty()) && sig->algorithm != sig_alg_hs2019) {
            LOG_WARN("Unsupported HTTP signature algorithm for matching key: ", sig->algorithm);
            continue;
        }
        sig_found = true;
        if (!(sig->verify(rsh, pk)))
            continue;  // head does not match signature
        return true;
    }

    if (!sig_found)
        ec = asio::error::invalid_argument;
    return false;
}

std::string
http_key_id_for_injection(const ouinet::util::Ed25519PublicKey& pk)
{
    return "ed25519=" + ouinet::util::base64_encode(pk.serialize());
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
        trim_whitespace(value_v);

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
              , const std::string& key_id
              , std::chrono::seconds::rep ts)
{
    static const auto fmt_ = "keyId=\"%s\""
                             ",algorithm=\"" + sig_alg_hs2019 + "\""
                             ",created=%d"
                             ",headers=\"%s\""
                             ",signature=\"%s\"";
    boost::format fmt(fmt_);

    http::response_header<> sig_head;
    sig_head.set("(response-status)", rsh.result_int());
    sig_head.set("(created)", ts);
    prep_sig_head(rsh, sig_head);  // unique fields, lowercase names, trimmed values

    std::string sig_string, headers;
    std::tie(sig_string, headers) = get_sig_str_hdrs(sig_head);

    auto encoded_sig = ouinet::util::base64_encode(sk.sign(sig_string));

    return (fmt % key_id % ts % headers % encoded_sig).str();
}

}} // namespaces
