#include "routing_table.h"
#include "dht.h"
#include "proximity_map.h"

#include <set>

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent::dht;

RoutingTable::RoutingTable(const NodeID& node_id, SendPing send_ping)
    : _node_id(node_id)
    , _send_ping(move(send_ping))
    , _buckets(NodeID::bit_size)
{
}

size_t RoutingTable::bucket_id(const NodeID& id) const
{
    NodeID diff = _node_id ^ id;

    // Bucket 0 is one with all contacts with IDs same as ours (which means
    // this particular one will be empty, but that's the price to pay for not
    // needing to do branching on `if 0`).
    size_t bucket_i = 0;

    for (size_t i = 0; i < NodeID::bit_size; ++i) {
        if (diff.bit(i)) {
            bucket_i = NodeID::bit_size - i - 1;
            break;
        }
    }

    return bucket_i;
}

RoutingTable::Bucket* RoutingTable::find_bucket(NodeID id)
{
    return &_buckets[bucket_id(id)];
}

std::vector<NodeContact>
RoutingTable::find_closest_routing_nodes(NodeID target, size_t count)
{
    std::vector<NodeContact> output;

    ProximityMap<NodeContact> m(target, count);

    // TODO: This isn't efficient. Instead of going through every single bucket
    // and try to add it to `m`, we should start in the bucket that corresponds
    // to `target` and then move to both sides until no new contacts fit into `m`.
    for (auto& bucket : _buckets) {
        for (auto & n : bucket.nodes) {
            m.insert({n.contact.id, n.contact});
        }
    }

    for (auto& p : m) {
        output.push_back(p.second);
    }

    return output;
}

template<class R, class P>
static void erase_if(R& r, P&& p)
{
    r.erase( std::remove_if(std::begin(r), std::end(r), std::forward<P>(p))
           , std::end(r));
}

template<class Q>
static void erase_front_questionables(Q& q)
{
    while (!q.empty() && q[0].is_questionable()) {
        q.pop_front();
    }
}

/*
 * Record a node in the routing table, space permitting. If there is no space,
 * check for node replacement opportunities. If is_verified is not set, ping
 * the target contact before adding it.
 */
void RoutingTable::try_add_node(NodeContact contact, bool is_verified)
{
    Bucket* bucket = find_bucket(contact.id);

    auto now = Clock::now();

    /*
     * Check whether the contact is already in the routing table. If so, bump it.
     */
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].contact == contact) {
            RoutingNode node = bucket->nodes[i];

            node.recv_time = now;

            if (is_verified) {
                node.reply_time     = now;
                node.queries_failed = 0;
                node.ping_ongoing   = false;
            }

            bucket->nodes.erase(bucket->nodes.begin() + i);
            bucket->nodes.push_back(node);
            return;
        }
    }

    erase_if(bucket->verified_candidates,   [&] (auto& c) { return c.contact == contact; });
    erase_if(bucket->unverified_candidates, [&] (auto& c) { return c.contact == contact; });

    /*
     * If there is space in the bucket, add the node. If it is unverified,
     * ping it instead; on success, the node will be added.
     */
    if (bucket->nodes.size() < BUCKET_SIZE) {
        if (is_verified) {
            bucket->nodes.push_back(RoutingNode {
                .contact        = contact,
                .recv_time      = now,
                .reply_time     = now,
                .queries_failed = 0,
                .ping_ongoing   = false,
            });
        } else {
            _send_ping(contact);
        }
        return;
    }

    /*
     * Check whether there are any bad nodes in the table. If so, replace it,
     * per above.
     */
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (!bucket->nodes[i].is_good()) {
            if (is_verified) {
                bucket->nodes.erase(bucket->nodes.begin() + i);

                bucket->nodes.push_back(RoutingNode {
                    .contact        = contact,
                    .recv_time      = now,
                    .reply_time     = now,
                    .queries_failed = 0,
                    .ping_ongoing   = false,
                });
            } else {
                _send_ping(contact);
            }
            return;
        }
    }

    /*
     * Count the number of questionable nodes, and make sure they are all being
     * pinged to check whether they are still good.
     */
    size_t questionable_nodes = 0;

    for (auto& n : bucket->nodes) {
        if (n.is_questionable()) {
            questionable_nodes++;
            if (!n.ping_ongoing) {
                _send_ping(n.contact);
                n.ping_ongoing = true;
            }
        }
    }

    /*
     * Add the contact as a candidate.
     */
    RoutingNode candidate {
        .contact        = contact,
        .recv_time      = now,
        .reply_time     = is_verified ? now : Clock::time_point(),
        .queries_failed = 0,
        .ping_ongoing   = false
    };

    if (is_verified) {
        if (questionable_nodes > 0) {
            bucket->verified_candidates.push_back(candidate);
        }
    } else {
        /*
         * An unverified contact can either replace other unverified contacts,
         * or verified contacts that have become questionable (read: old).
         */
        erase_front_questionables(bucket->verified_candidates);

        if (bucket->verified_candidates.size() < questionable_nodes) {
            bucket->unverified_candidates.push_back(candidate);
        }
    }

    while (bucket->verified_candidates.size() > questionable_nodes) {
        bucket->verified_candidates.pop_front();
    }

    while (bucket->verified_candidates.size() + bucket->unverified_candidates.size() > questionable_nodes) {
        bucket->unverified_candidates.pop_front();
    }
}

/*
 * Record a failure of a routing table node to respond to a query. If this
 * makes the node bad, try to replace it with a queued candidate.
 */
void RoutingTable::fail_node(NodeContact contact)
{
    Bucket* bucket = find_bucket(contact.id);

    /*
     * Find the contact in the routing table.
     */
    size_t node_i = bucket->nodes.size();

    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].contact == contact) {
            node_i = i;
            break;
        }
    }

    if (node_i == bucket->nodes.size()) return;

    bucket->nodes[node_i].queries_failed++;

    if (bucket->nodes[node_i].is_good()) {
        if (bucket->nodes[node_i].is_questionable()) {
            bucket->nodes[node_i].ping_ongoing = true;
            _send_ping(contact);
        }
        return;
    }

    /*
     * The node is bad. Try to replace it with one of the queued replacements.
     */
    erase_front_questionables(bucket->verified_candidates);
    erase_front_questionables(bucket->unverified_candidates);

    if (!bucket->verified_candidates.empty()) {
        /*
         * If there is a verified candidate available, use it.
         */
        bucket->nodes.erase(bucket->nodes.begin() + node_i);

        auto c = bucket->verified_candidates[0];
        bucket->verified_candidates.pop_front();

        RoutingNode node {
            .contact        = c.contact,
            .recv_time      = c.recv_time,
            .reply_time     = c.reply_time,
            .queries_failed = 0,
            .ping_ongoing   = false
        };

        for (size_t i = 0; i < bucket->nodes.size(); i++) {
            if (bucket->nodes[i].recv_time > node.recv_time) {
                bucket->nodes.insert(bucket->nodes.begin() + i, node);
                break;
            }
        }
    } else if (!bucket->unverified_candidates.empty()) {
        /*
         * If there is an unverified candidate available, ping it. The reply
         * handler will replace the bad node.
         */
        NodeContact contact = bucket->unverified_candidates[0].contact;
        bucket->unverified_candidates.pop_front();
        _send_ping(contact);
    }

    /*
     * Cleanup superfluous candidates.
     */
    size_t questionable_nodes = 0;

    for (auto& n : bucket->nodes) {
        if (n.is_questionable()) questionable_nodes++;
    }

    while (bucket->verified_candidates.size() > questionable_nodes) {
        bucket->verified_candidates.pop_front();
    }

    while (bucket->verified_candidates.size() + bucket->unverified_candidates.size() > questionable_nodes) {
        bucket->unverified_candidates.pop_front();
    }
}

