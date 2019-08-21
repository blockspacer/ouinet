#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>

#include "../constants.h"
#include "../util/crypto.h"
#include "../util/hash.h"

#include "../namespaces.h"

namespace ouinet { namespace http_ {
    // A prefix for HTTP signature headers at the response head,
    // each of them followed by a non-repeating, 0-based decimal integer.
    static const std::string response_signature_hdr_pfx = header_prefix + "Sig";
}}

namespace ouinet { namespace cache {

// Get an extended version of the given response head
// with an additional signature header and
// other headers required to support that signature and
// a future one for the full message head (as part of the trailer).
//
// Example:
//
//     ...
//     X-Ouinet-Version: 0
//     X-Ouinet-URI: https://example.com/foo
//     X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
//     X-Ouinet-Sig0: keyId="...",algorithm="hs2019",created=1516048310,
//       headers="(response-status) (created) ... x-ouinet-injection",
//       signature="..."
//     Transfer-Encoding: chunked
//     Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1
//
http::response_header<>  // use this to enable setting the time stamp (e.g. for tests)
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , std::chrono::seconds::rep injection_ts
                   , const ouinet::util::Ed25519PrivateKey&
                   , const std::string& key_id);

inline
http::response_header<>  // use this for the rest of cases
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , const ouinet::util::Ed25519PrivateKey& sk
                   , const std::string& key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_injection_head(rqh, std::move(rsh), injection_id, ts, sk, key_id);
}

// Get an extended version of the given response trailer
// with added headers completing the signature of the message.
//
// Please note that framing headers (`Content-Length`, `Transfer-Encoding`, `Trailer`)
// are not included in the signature, though an `X-Ouinet-Data-Size` header is added to
// convey the actual content length after the whole content has been seen.
// If a non-chunked response head needs to be constructed from the signed head,
// a `Content-Length` header should be added with the value of `X-Ouinet-Data-Size`
// (and the later be kept as well to avoid a signature verification failure).
//
// The signature of the initial head (`X-Ouinet-Sig0`) is not included among
// the signed headers, so that the receiver may replace it with
// the value of the signature in the trailer (`X-Ouinet-Sig1`)
// for subsequent uses.
//
// Example:
//
//     ...
//     X-Ouinet-Data-Size: 38
//     Digest: SHA-256=j7uwtB/QQz0FJONbkyEmaqlJwGehJLqWoCO1ceuM30w=
//     X-Ouinet-Sig1: keyId="...",algorithm="hs2019",created=1516048311,
//       headers="(response-status) (created) ... x-ouinet-injection x-ouinet-data-size digest",
//       signature="..."
//
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey&
                      , const std::string& key_id
                      , std::chrono::seconds::rep ts);

inline
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey& sk
                      , const std::string& key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_injection_trailer( rsh, std::move(rst)
                                 , content_length, content_digest
                                 , sk, key_id, ts);
}

// Verify that the given response head contains
// a valid signature for it from the given public key.
// If such a signature exists, return true or false depending on
// whether the head matches the signature.
// If any other error happens, set the error code.
bool
http_injection_verify( const http::response_header<>&
                     , const ouinet::util::Ed25519PublicKey&
                     , sys::error_code&);

// Get a `keyId` encoding the given public key itself.
std::string
http_key_id_for_injection(const ouinet::util::Ed25519PublicKey&);

// Get the body digest as per RFC 3230 and RFC 5843.
//
// Example:
//
//     SHA-256=NYfLd2zg5OgjfyFYALff+6DyWGXLhFUOh+qLusg4xCM=
//
std::string
http_digest(const http::response<http::dynamic_body>&);

// Compute a signature as per draft-cavage-http-signatures-11.
std::string  // use this to enable setting the time stamp (e.g. for tests)
http_signature( const http::response_header<>&
              , const ouinet::util::Ed25519PrivateKey&
              , const std::string& key_id
              , std::chrono::seconds::rep ts);

inline  // use this for the rest of cases
std::string
http_signature( const http::response_header<>& rsh
              , const ouinet::util::Ed25519PrivateKey& sk
              , const std::string& key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_signature(rsh, sk, key_id, ts);
}

}} // namespaces
