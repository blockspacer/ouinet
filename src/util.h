#pragma once

#include <fstream>
#include <string>

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/utility/string_view.hpp>

#include "namespaces.h"
#include "util/signal.h"
#include "util/condition_variable.h"
#include "or_throw.h"

namespace ouinet { namespace util {

struct url_match {
    std::string scheme;
    std::string host;
    std::string port;  // maybe empty
    std::string path;
    std::string query;  // maybe empty
    std::string fragment;  // maybe empty
};

// Parse the HTTP URL to tell the different components.
// If successful, the `match` is updated.
inline
bool match_http_url(const boost::string_view url, url_match& match) {
    static const boost::regex urlrx( "^(http|https)://"  // 1: scheme
                                     "([-\\.a-z0-9]+|\\[[:0-9a-fA-F]+\\])"  // 2: host
                                     "(:[0-9]{1,5})?"  // 3: :port (or empty)
                                     "(/[^?#]*)"  // 4: /path
                                     "(\\?[^#]*)?"  // 5: ?query (or empty)
                                     "(#.*)?");  // 6: #fragment (or empty)
    boost::cmatch m;
    if (!boost::regex_match(url.begin(), url.end(), m, urlrx))
        return false;
    match = { m[1]
            , m[2]
            , m[3].length() > 0 ? std::string(m[3], 1) : ""  // drop colon
            , m[4]
            , m[5].length() > 0 ? std::string(m[5], 1) : ""  // drop qmark
            , m[6].length() > 0 ? std::string(m[6], 1) : ""  // drop hash
    };
    return true;
}

inline
asio::ip::tcp::endpoint
parse_tcp_endpoint(const std::string& s, sys::error_code& ec)
{
    using namespace std;
    auto pos = s.rfind(':');

    ec = sys::error_code();

    if (pos == string::npos) {
        ec = asio::error::invalid_argument;
        return asio::ip::tcp::endpoint();
    }

    auto addr = asio::ip::address::from_string(s.substr(0, pos));

    auto pb = s.c_str() + pos + 1;
    uint16_t port = std::atoi(pb);

    if (port == 0 && !(*pb == '0' && *(pb+1) == 0)) {
        ec = asio::error::invalid_argument;
        return asio::ip::tcp::endpoint();
    }

    return asio::ip::tcp::endpoint(move(addr), port);
}

inline
asio::ip::tcp::endpoint
parse_tcp_endpoint(const std::string& s)
{
    sys::error_code ec;
    auto ep = parse_tcp_endpoint(s, ec);
    if (ec) throw sys::system_error(ec);
    return ep;
}

inline
auto tcp_async_resolve( const std::string& host
                      , const std::string& port
                      , asio::io_service& ios
                      , Cancel& cancel
                      , asio::yield_context yield)
{
    using tcp = asio::ip::tcp;
    using Results = tcp::resolver::results_type;

    if (cancel) {
        return or_throw<Results>(yield, asio::error::operation_aborted);
    }

    // Note: we're spawning a new coroutine here and deal with all this
    // ConditionVariable machinery because - contrary to what Asio's
    // documentation says - resolver::async_resolve isn't immediately
    // cancelable. I.e.  when resolver::async_resolve is running and
    // resolver::cancel is called, it is not guaranteed that the async_resolve
    // call gets placed on the io_service queue immediately. Instead, it was
    // observed that this can in some rare cases take more than 20 seconds.
    //
    // Also note that this is not Asio's fault. Asio uses internally the
    // getaddrinfo() function which doesn't support cancellation.
    //
    // https://stackoverflow.com/questions/41352985/abort-a-call-to-getaddrinfo
    sys::error_code ec;
    Results results;
    ConditionVariable cv(ios);
    tcp::resolver* rp = nullptr;

    auto cancel_lookup_slot = cancel.connect([&] {
        ec = asio::error::operation_aborted;
        cv.notify();
        if (rp) rp->cancel();
    });

    bool* finished_p = nullptr;

    asio::spawn(ios, [&] (asio::yield_context yield) {
        bool finished = false;
        finished_p = &finished;

        tcp::resolver resolver{ios};
        rp = &resolver;
        sys::error_code ec_;
        auto r = resolver.async_resolve({host, port}, yield[ec_]);

        if (finished) return;

        rp = nullptr;
        results = std::move(r);
        ec = ec_;
        finished_p = nullptr;
        cv.notify();
    });

    cv.wait(yield);

    if (finished_p) *finished_p = true;

    return or_throw(yield, ec, std::move(results));
}

// Return whether the given `host` points to a loopback address.
// IPv6 addresses should not be bracketed.
inline
bool is_localhost(const std::string& host)
{
    // Fortunately, resolving also canonicalizes IPv6 addresses
    // so we can simplify the regular expression.`:)`
    static const std::string ip4loopre = "127(?:\\.[0-9]{1,3}){3}";
    static const std::string lhre =
        std::string()
        + "^(?:"
        + "(?:localhost|ip6-localhost|ip6-loopback)(?:\\.localdomain)?"
        + "|" + ip4loopre         // IPv4, e.g. 127.1.2.3
        + "|::1"                  // IPv6 loopback
        + "|::ffff:" + ip4loopre  // IPv4-mapped IPv6
        + "|::" + ip4loopre       // IPv4-compatible IPv6
        + ")$";
    static const boost::regex lhrx(lhre);

    // Avoid the DNS lookup for very evident loopback addresses.`;)`
    boost::smatch m;
    if (boost::regex_match(host, m, lhrx))
        return true;

    return false;
}

// Format host/port pair taking IPv6 into account.
inline
std::string format_ep(const std::string& host, const std::string& port) {
    return ( (host.find(':') == std::string::npos
              ? host // IPv4/name
              : "[" + host + "]")  // IPv6
             + ":" + port);
}

inline
std::string format_ep(const asio::ip::tcp::endpoint& ep) {
    return format_ep(ep.address().to_string(), std::to_string(ep.port()));
}

///////////////////////////////////////////////////////////////////////////////
std::string zlib_compress(const std::string&);
std::string zlib_decompress(const std::string&, sys::error_code&);
std::string base64_encode(const std::string&);

///////////////////////////////////////////////////////////////////////////////
namespace detail {
inline
std::string str_impl(std::stringstream& ss) {
    return ss.str();
}

template<class Arg, class... Args>
inline
std::string str_impl(std::stringstream& ss, Arg&& arg, Args&&... args) {
    ss << arg;
    return str_impl(ss, std::forward<Args>(args)...);
}

} // detail namespace

template<class... Args>
inline
std::string str(Args&&... args) {
    std::stringstream ss;
    return detail::str_impl(ss, std::forward<Args>(args)...);
}

///////////////////////////////////////////////////////////////////////////////
// Write a small file at the given `path` with a `line` of content.
// If existing, truncate it.
inline
void create_state_file(const boost::filesystem::path& path, const std::string& line) {
    std::fstream fs(path.native(), std::fstream::out | std::fstream::trunc);
    fs << line << std::endl;
    fs.close();
}

///////////////////////////////////////////////////////////////////////////////

}} // ouinet::util namespace
