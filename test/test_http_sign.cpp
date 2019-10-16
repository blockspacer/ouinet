#define BOOST_TEST_MODULE http_sign
#include <boost/test/included/unit_test.hpp>

#include <sstream>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include <util.h>
#include <util/bytes.h>
#include <util/connected_pair.h>
#include <util/crypto.h>
#include <util/wait_condition.h>
#include <util/yield.h>
#include <cache/http_sign.h>
#include <http_forward.h>
#include <session.h>

#include <namespaces.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_sign)

using namespace std;
using namespace ouinet;

static const string rq_target = "https://example.com/foo";  // proxy-like
static const string rq_host = "example.com";

static const string rs_body =
  ( "0123" + string(http_::response_data_block - 8, 'x') + "4567"
  + "89AB" + string(http_::response_data_block - 8, 'x') + "CDEF"
  + "abcd");
static const string rs_body_b64digest = "E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=";
static const string rs_head_s = (
    "HTTP/1.1 200 OK\r\n"
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
    "Content-Length: 131076\r\n"
    "Server: Apache2\r\n"
    "\r\n"
);
static const string inj_id = "d6076384-2295-462b-a047-fe2c9274e58d";
static const std::chrono::seconds::rep inj_ts = 1516048310;
static const string inj_b64sk = "MfWAV5YllPAPeMuLXwN2mUkV9YaSSJVUcj/2YOaFmwQ=";
static const string inj_b64pk = "DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=";

// If Beast changes message representation or shuffles headers,
// the example will need to be updated,
// but the signature should stay the same.
// If comparing the whole head becomes too tricky, just check `X-Ouinet-Sig0`.
static const string rs_head_signed_s = (
    "HTTP/1.1 200 OK\r\n"
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Server: Apache2\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"

    "X-Ouinet-Version: 0\r\n"
    "X-Ouinet-URI: https://example.com/foo\r\n"
    "X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310\r\n"
    "X-Ouinet-BSigs: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",size=65536\r\n"

    "X-Ouinet-Sig0: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048310,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs\","
    "signature=\"tqF93Xjif/4bZ94XN2jzGeURJT8x0TznfKuZCeG4aE5pigZDaHWESKJhwhKevlDqgjNdI6XdWG4WcaLRWhXXCg==\"\r\n"

    "Transfer-Encoding: chunked\r\n"
    "Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1\r\n"

    "X-Ouinet-Data-Size: 131076\r\n"
    "Digest: SHA-256=E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=\r\n"

    "X-Ouinet-Sig1: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048311,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs "
    "x-ouinet-data-size "
    "digest\","
    "signature=\"ouLm95hwtXdshodDm/ncF9ZHX5cEvygG+ReokuKUc6K0AGg/rPuBM46vY2BG+Qox8doHFKKDF526v5JFcif8CA==\"\r\n"
    "\r\n"
);

static const array<string, 3> rs_block_cexts{
    ";ouisig=\"6gCnxL3lVHMAMSzhx+XJ1ZBt+JC/++m5hlak1adZMlUH0hnm2S3ZnbwjPQGMm9hDB45SqnybuQ9Bjo+PgnfnCw==\"",  // offset 0
    ";ouisig=\"5R+TDVurWEZCPRxTBF0s7uDwxcbfxjj35Xr101C2ZDKyLbHw5tHIvhrJTjpfUIQKQ99OD5Xr08Q7j2fpQuoVDg==\"",  // offset 65536
    ";ouisig=\"B1LsKycoNZMGF3AdPegJySkEF42sp456P7yX2/75/T/ZlbP2UqyjKA7Bqy7SwGBTtms5g7ckQOMKbzT9KfT1Cg==\"",  // offset 131072
};

template<class F>
static void run_spawned(asio::io_service& ios, F&& f) {
    asio::spawn(ios, [&ios, f = forward<F>(f)] (auto yield) {
            try {
                f(Yield(ios, yield));
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });
    ios.run();
}

static http::request_header<> get_request_header() {
    http::request_header<> req_h;
    req_h.method(http::verb::get);
    req_h.target(rq_target);
    req_h.version(11);
    req_h.set(http::field::host, rq_host);

    return req_h;
}

static util::Ed25519PrivateKey get_private_key() {
    auto ska = util::bytes::to_array<uint8_t, util::Ed25519PrivateKey::key_size>(util::base64_decode(inj_b64sk));
    return util::Ed25519PrivateKey(std::move(ska));
}

BOOST_AUTO_TEST_CASE(test_http_sign) {

    sys::error_code ec;

    const auto digest = util::sha256_digest(rs_body);
    const auto b64_digest = util::base64_encode(digest);
    BOOST_REQUIRE(b64_digest == rs_body_b64digest);

    http::response_parser<http::string_body> parser;
    parser.put(asio::buffer(rs_head_s), ec);
    BOOST_REQUIRE(!ec);
    parser.put(asio::buffer(rs_body), ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_done());
    auto rs_head = parser.get().base();

    auto req_h = get_request_header();

    const auto sk = get_private_key();
    const auto key_id = cache::http_key_id_for_injection(sk.public_key());
    BOOST_REQUIRE(key_id == ("ed25519=" + inj_b64pk));

    rs_head = cache::http_injection_head(req_h, std::move(rs_head), inj_id, inj_ts, sk, key_id);

    http::fields trailer;
    trailer = cache::http_injection_trailer( rs_head, std::move(trailer)
                                           , rs_body.size(), digest
                                           , sk, key_id, inj_ts + 1);
    // Add headers from the trailer to the injection head.
    for (auto& hdr : trailer)
        rs_head.set(hdr.name_string(), hdr.value());

    std::stringstream rs_head_ss;
    rs_head_ss << rs_head;
    BOOST_REQUIRE(rs_head_ss.str() == rs_head_signed_s);

}

// Put everything in the string to the given parser,
// until everything is parsed or some error happens.
template<class Parser>
static
void put_to_parser(Parser& p, const string& s, sys::error_code& ec) {
    auto b = asio::const_buffer(s.data(), s.size());
    while (b.size() > 0) {
        auto consumed = p.put(b, ec);
        if (ec) return;
        b += consumed;
    };
}

BOOST_AUTO_TEST_CASE(test_http_verify) {

    sys::error_code ec;

    http::response_parser<http::string_body> parser;
    put_to_parser(parser, rs_head_signed_s, ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_header_done());
    BOOST_REQUIRE(parser.chunked());
    // The signed response head signals chunked transfer encoding.
    auto rs_body_s = ( beast::buffers_to_string(http::make_chunk(asio::buffer(rs_body)))
                     // We should really be adding the trailer here,
                     // but it is already part of `rs_head_signed_s`.
                     // Beast seems to be fine with that, though.
                     + beast::buffers_to_string(http::make_chunk_last()));
    put_to_parser(parser, rs_body_s, ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_done());
    auto rs_head_signed = parser.get().base();

    const auto pka = util::bytes::to_array<uint8_t, util::Ed25519PublicKey::key_size>(util::base64_decode(inj_b64pk));
    const util::Ed25519PublicKey pk(std::move(pka));
    const auto key_id = cache::http_key_id_for_injection(pk);
    BOOST_REQUIRE(key_id == ("ed25519=" + inj_b64pk));

    // Add an unexpected header.
    // It should not break signature verification, but it should be removed from its output.
    rs_head_signed.set("X-Foo", "bar");
    // Move a header, keeping the same value.
    // It should not break signature verification.
    auto date = rs_head_signed[http::field::date].to_string();
    rs_head_signed.erase(http::field::date);
    rs_head_signed.set(http::field::date, date);

    auto vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() != vfy_res.cend());  // successful verification
    BOOST_REQUIRE(vfy_res["X-Foo"].empty());
    // TODO: check same headers

    // Add a bad third signature (by altering the second one).
    // It should not break signature verification, but it should be removed from its output.
    auto sig1_copy = rs_head_signed["X-Ouinet-Sig1"].to_string();
    string sstart(",signature=\"");
    auto spos = sig1_copy.find(sstart);
    BOOST_REQUIRE(spos != string::npos);
    sig1_copy.replace(spos + sstart.length(), 7, "GARBAGE");  // change signature
    rs_head_signed.set("X-Ouinet-Sig2", sig1_copy);

    vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() != vfy_res.cend());  // successful verification
    BOOST_REQUIRE(vfy_res["X-Ouinet-Sig2"].empty());

    // Change the key id of the third signature to refer to some other key.
    // It should not break signature verification, and it should be kept in its output.
    auto kpos = sig1_copy.find(inj_b64pk);
    BOOST_REQUIRE(kpos != string::npos);
    sig1_copy.replace(kpos, 7, "GARBAGE");  // change keyId
    rs_head_signed.set("X-Ouinet-Sig2", sig1_copy);

    vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() != vfy_res.cend());  // successful verification
    BOOST_REQUIRE(!vfy_res["X-Ouinet-Sig2"].empty());
    // TODO: check same headers

    // Alter the value of one of the signed headers and verify again.
    // It should break signature verification.
    rs_head_signed.set(http::field::server, "NginX");
    vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() == vfy_res.cend());  // unsuccessful verification

}

BOOST_AUTO_TEST_CASE(test_http_flush_signed) {
    asio::io_service ios;
    run_spawned(ios, [&] (auto yield) {
        WaitCondition wc(ios);

        asio::ip::tcp::socket
            origin_w(ios), origin_r(ios),
            signed_w(ios), signed_r(ios),
            tested_w(ios), tested_r(ios);
        tie(origin_w, origin_r) = util::connected_pair(ios, yield);
        tie(signed_w, signed_r) = util::connected_pair(ios, yield);
        tie(tested_w, tested_r) = util::connected_pair(ios, yield);

        // Send raw origin response.
        asio::spawn(ios, [&origin_w, &ios, lock = wc.lock()] (auto y) {
            sys::error_code e;
            asio::async_write( origin_w
                             , asio::const_buffer(rs_head_s.data(), rs_head_s.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            asio::async_write( origin_w
                             , asio::const_buffer(rs_body.data(), rs_body.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            origin_w.close();
        });

        // Sign origin response.
        asio::spawn(ios, [ origin_r = std::move(origin_r), &signed_w
                         , &ios, lock = wc.lock()] (auto y) mutable {
            Session origin_rs(std::move(origin_r));
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Cancel cancel;
            sys::error_code e;
            cache::session_flush_signed( origin_rs, signed_w
                                       , req_h, inj_id, inj_ts, sk
                                       , cancel, y[e]);
            BOOST_REQUIRE(!e);
            signed_w.close();
        });

        // Test signed output.
        asio::spawn(ios, [ &signed_r, &tested_w
                         , &ios, lock = wc.lock()](auto y) mutable {
            auto hproc = [] (auto inh, auto&, auto) {
                auto hbsh = inh[http_::response_block_signatures_hdr];
                BOOST_REQUIRE(!hbsh.empty());
                auto hbs = cache::HttpBlockSigs::parse(hbsh);
                BOOST_REQUIRE(hbs);
                // Test data block signatures are split according to this size.
                BOOST_CHECK_EQUAL(hbs->size, 65536);
                return inh;
            };
            int xidx = 0;
            auto xproc = [&xidx] (auto exts, auto&, auto) {
                BOOST_REQUIRE(xidx < rs_block_cexts.size());
                BOOST_CHECK_EQUAL(exts, rs_block_cexts[xidx++]);
            };
            // Yes we drop chunk extensions but they do not affect the forwarding process,
            // and they are going to be dumped anyway further down.
            ProcDataFunc<asio::const_buffer> dproc = [] (auto ind, auto&, auto) {
                return ProcDataFunc<asio::const_buffer>::result_type{std::move(ind), {}};
            };
            ProcTrailFunc tproc = [] (auto intr, auto&, auto) {
                return ProcTrailFunc::result_type{std::move(intr), {}};
            };

            Cancel cancel;
            sys::error_code e;
            Yield yy(ios, y);
            http_forward( signed_r, tested_w
                        , std::move(hproc), std::move(xproc)
                        , std::move(dproc), std::move(tproc)
                        , cancel, yy[e]);
            BOOST_REQUIRE(!e);
            BOOST_CHECK_EQUAL(xidx, rs_block_cexts.size());
            signed_r.close();
            tested_w.close();
        });

        // Black hole.
        asio::spawn(ios, [&tested_r, &ios, lock = wc.lock()] (auto y) {
            char d[2048];
            asio::mutable_buffer b(d, sizeof(d));

            sys::error_code e;
            while (!e) asio::async_read(tested_r, b, y[e]);
            BOOST_REQUIRE(e == asio::error::eof || !e);
            tested_r.close();
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_SUITE_END()
