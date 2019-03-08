#pragma once

#include <list>
#include <cstddef>
#include <stdexcept>
#include <boost/filesystem.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "../namespaces.h"
#include "../defer.h"
#include "signal.h"
#include "file_io.h"
#include "scheduler.h"
#include "sha1.h"
#include "bytes.h"

namespace ouinet { namespace util {

namespace persisten_lru_cache_detail {
    void create_or_check_directory(const fs::path&, sys::error_code&);
    uint64_t ms_since_epoch();
} // detail namespace

class PersistentLruCache {
private:
    struct Element;

    using Key = std::string;
    using KeyVal = std::pair<Key, std::shared_ptr<Element>>;

    using List = typename std::list<KeyVal>;
    using ListIter = typename List::iterator;

    using Map = std::map<Key, ListIter>;
    using MapIter = typename Map::iterator;

public:
    class iterator {
        friend class PersistentLruCache;
        MapIter i;
    public:
        iterator(MapIter i) : i(i) {}

        iterator& operator++() {
            ++i;
            return *this;
        }

        iterator operator++(int) {
            iterator ret{i};
            ++i;
            return ret;
        }

        bool operator==(iterator j) const {
            return i == j.i;
        }

        bool operator!=(iterator j) const {
            return i != j.i;
        }

        std::string value(Cancel&, asio::yield_context);
    };

private:
    /* private, use the static `load` function */
    PersistentLruCache( asio::io_service&
                      , boost::filesystem::path dir
                      , size_t max_size);

public:
    PersistentLruCache(const PersistentLruCache&) = delete;
    PersistentLruCache(PersistentLruCache&&) = delete;

    static
    std::unique_ptr<PersistentLruCache> load( asio::io_service&
                                            , boost::filesystem::path dir
                                            , size_t max_size
                                            , Cancel&
                                            , asio::yield_context);

    void insert( std::string key
               , std::string value
               , Cancel&
               , asio::yield_context);

    iterator find(const std::string& key);

    bool exists(const std::string& key) const {
        return _map.count(key) != 0;
    }

    size_t size() const {
        return _map.size();
    }

    bool empty() const { return _map.empty(); }

    // TODO: Currently the returned iterator is not
    // ordered by usage.
    iterator begin() {
        return iterator(_map.begin());
    }

    // TODO: Currently the returned iterator is not
    // ordered by usage.
    iterator end() {
        return iterator(_map.end());
    }

    void move_to_front(iterator i) {
        _list.splice(_list.begin(), _list, i.i->second);
    }

    const boost::filesystem::path& dir() const {
        return _dir;
    }

private:
    fs::path path_from_key(const std::string&);

private:
    asio::io_service& _ios;
    boost::filesystem::path _dir;
    List _list;
    Map _map;
    size_t _max_size;
};

class PersistentLruCache::Element {
public:
    static
    std::shared_ptr<Element> open( asio::io_service& ios
                                 , fs::path path
                                 , uint64_t* ts_out
                                 , Cancel& cancel
                                 , asio::yield_context yield)
    {
        using Ret = std::shared_ptr<Element>;

        sys::error_code ec;

        auto on_exit = defer([&] { if (ec) file_io::remove_file(path); });

        auto file = file_io::open(ios, path, ec);
        if (ec) return or_throw<Ret>(yield, ec);

        auto ts = file_io::read_number<uint64_t>(file, cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);

        if (ts_out) *ts_out = ts;

        auto key_size = file_io::read_number<uint32_t>(file, cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);

        std::string key(key_size, '\0');
        file_io::read(file, asio::buffer(key), cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);

        return std::make_shared<Element>(ios, std::move(key), std::move(path));
    }

    void update( std::string value
               , Cancel& cancel
               , asio::yield_context yield)
    {
        using namespace persisten_lru_cache_detail;

        auto ts = ms_since_epoch();

        sys::error_code ec;

        auto f = file_io::open(_ios, _path, ec);
        if (!ec) file_io::truncate(f, content_start() + value.size(), ec);
        if (!ec) file_io::fseek(f, 0, ec);
        if (!ec) file_io::write_number<uint64_t>(f, ts, cancel, yield[ec]);
        if (!ec) file_io::write_number<uint32_t>(f, _key.size(), cancel, yield[ec]);
        if (!ec) file_io::write(f, asio::buffer(_key), cancel, yield[ec]);
        if (!ec) file_io::write(f, asio::buffer(value), cancel, yield[ec]);

        if (ec) _remove_on_destruct = true;
        else _remove_on_destruct = false;

        return or_throw(yield, ec);
    }

    void update(Cancel& cancel, asio::yield_context yield)
    {
        using namespace persisten_lru_cache_detail;

        auto ts = ms_since_epoch();

        sys::error_code ec;

        auto f = file_io::open(_ios, _path, ec);
        if (!ec) file_io::fseek(f, 0, ec);
        if (!ec) file_io::write_number<uint64_t>(f, ts, cancel, yield[ec]);

        if (ec) _remove_on_destruct = true;
        else _remove_on_destruct = false;

        return or_throw(yield, ec);
    }

    std::string value(Cancel& cancel, asio::yield_context yield)
    {
        using namespace persisten_lru_cache_detail;

        auto ts = ms_since_epoch();

        sys::error_code ec;

        size_t f_size = fs::file_size(_path, ec);
        std::string ret;

        auto f = file_io::open(_ios, _path, ec);
        if (ec) goto finish;

        file_io::fseek(f, 0, ec);
        if (ec) goto finish;

        file_io::write_number<uint64_t>(f, ts, cancel, yield[ec]);
        if (ec) goto finish;

        file_io::fseek(f, content_start(), ec);
        if (ec) goto finish;

        ret.resize(f_size - content_start());
        file_io::read(f, asio::buffer(ret), cancel, yield[ec]);

        finish:

        if (ec) _remove_on_destruct = true;
        else _remove_on_destruct = false;

        return or_throw(yield, ec, std::move(ret));
    }

    ~Element()
    {
        if (_remove_on_destruct) file_io::remove_file(_path);
    }

    Element( asio::io_service& ios
           , std::string key
           , fs::path path)
        : _ios(ios)
        , _scheduler(ios, 1)
        , _key(std::move(key))
        , _path(std::move(path))
    {}

    void remove_file_on_destruct() {
        _remove_on_destruct = true;
    }

    Scheduler::Slot lock(Cancel& cancel, asio::yield_context yield)
    {
        return _scheduler.wait_for_slot(cancel, yield);
    }

    const std::string& key() const { return _key; }

private:
    size_t content_start() const {
        return sizeof(uint64_t) // time stamp
             + sizeof(uint32_t) // key size
             + _key.size();
    }

private:
    asio::io_service& _ios;
    Scheduler _scheduler;
    std::string _key;
    fs::path _path;
    bool _remove_on_destruct = false;
};

inline
std::unique_ptr<PersistentLruCache>
PersistentLruCache::load( asio::io_service& ios
                        , boost::filesystem::path dir
                        , size_t max_size
                        , Cancel& cancel
                        , asio::yield_context yield)
{
    using namespace persisten_lru_cache_detail;

    using Ret = std::unique_ptr<PersistentLruCache>;

    sys::error_code ec;

    if (!dir.is_absolute()) {
        dir = fs::absolute(dir);
    }

    if (!ec) create_or_check_directory(dir, ec);
    if (ec) return or_throw<Ret>(yield, ec);

    Ret lru(new PersistentLruCache(ios, dir, max_size));

    // Id helps us resolve the case when two entries have the same timestamp
    using Id = std::pair<uint64_t, uint64_t>;

    std::map<Id, std::shared_ptr<Element>> elements;

    uint64_t i = 0;
    for (auto file : fs::directory_iterator(dir)) {
        uint64_t ts;
        auto e = Element::open(ios, file, &ts, cancel, yield[ec]);

        if (cancel) {
            return or_throw<Ret>(yield, asio::error::operation_aborted);
        }

        if (ec) continue;

        elements.insert({Id{ts, i++}, e});
    }

    while (elements.size() > max_size) {
        auto i = elements.begin();
        i->second->remove_file_on_destruct();
        elements.erase(i);
    }

    for (auto p : elements) {
        auto e = p.second;

        auto map_i = lru->_map.find(e->key());
        assert(map_i == lru->_map.end());
        lru->_list.push_front({e->key(), e});
        lru->_map[e->key()] = lru->_list.begin();
    }

    return lru;
}

inline
PersistentLruCache::PersistentLruCache( asio::io_service& ios
                                      , boost::filesystem::path dir
                                      , size_t max_size)
    : _ios(ios)
    , _dir(std::move(dir))
    , _max_size(max_size)
{
}

inline
void PersistentLruCache::insert( std::string key
                               , std::string value
                               , Cancel& cancel
                               , asio::yield_context yield)
{
    auto it = _map.find(key);

    std::shared_ptr<Element> e;

    if (it == _map.end()) {
        e = std::make_shared<Element>(_ios, key, path_from_key(key));
    } else {
        e = std::move(it->second->second);
    }

    _list.push_front({key, e});

    if (it != _map.end()) {
        it->second->second->remove_file_on_destruct();
        _list.erase(it->second);
        it->second = _list.begin();
    }
    else {
        it = _map.insert({key, _list.begin()}).first;
    }

    if (_map.size() > _max_size) {
        auto last = prev(_list.end());
        if (last->first == it->first) e = nullptr;
        last->second->remove_file_on_destruct();
        _map.erase(last->first);
        _list.pop_back();
    }

    if (!e) return;

    sys::error_code ec;
    auto slot = e->lock(cancel, yield[ec]);
    if (ec) return or_throw(yield, ec);
    e->update(std::move(value), cancel, yield);
}

inline
PersistentLruCache::iterator PersistentLruCache::find(const std::string& key)
{
    auto it = _map.find(key);

    if (it == _map.end()) return it;

    auto list_it = it->second;

    _list.splice(_list.begin(), _list, list_it);

    assert(list_it == _list.begin());

    return it;
}

inline
fs::path PersistentLruCache::path_from_key(const std::string& key)
{
    return _dir / util::bytes::to_hex(sha1(key));
}

inline
std::string
PersistentLruCache::iterator::value(Cancel& cancel, asio::yield_context yield)
{
    // Capture shared_ptr here to make sure element doesn't
    // get deleted while reading from the file.
    std::shared_ptr<Element> e = i->second->second;

    sys::error_code ec;

    auto lock = e->lock(cancel, yield[ec]);
    if (ec) return or_throw<std::string>(yield, ec);

    return e->value(cancel, yield);
}
}} // namespaces
