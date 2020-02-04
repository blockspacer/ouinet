#include "coro_tracker.h"
#include "logger.h"
#include <mutex>
#include <thread>
#include <chrono>

namespace ouinet {

using lock_guard = std::lock_guard<std::mutex>;
using namespace std::chrono;

enum State : unsigned {
    running = 0,
    stopped = 1,
    done = 2
};

struct CoroTracker::GlobalState {
    std::mutex mutex;
    std::thread thread;
    State state = running;
    List list;
    bool _keep_going = true;

    bool keep_going() {
        lock_guard guard(mutex);
        return _keep_going;
    }

    void stop() {
        {
            lock_guard guard(mutex);
            state = State::stopped;
            if (list.empty()) {
                state = done;
                return;
            }

            LOG_DEBUG("CoroTracker: Waiting for tracked coroutines to finish:");
            for (auto& e : list) {
                LOG_DEBUG("CoroTracker:    ", e.self->name());
            }
        }

        thread = std::thread([&] {
            auto step_duration = milliseconds(100);
            auto inform_after  = seconds(1);
            steady_clock::duration duration;

            while (keep_going()) {
                std::this_thread::sleep_for(step_duration);

                duration += step_duration;
                if (duration >= inform_after) break;

                {
                    lock_guard guard(mutex);
                    if (list.empty()) break;
                }
            }


            {
                lock_guard guard(mutex);
                if (list.empty()) {
                    LOG_INFO("CoroTracker: Done waiting for tracked coroutines");
                } else {
                    LOG_WARN("CoroTracker: Done waiting for tracked coroutines, "
                             "but some coroutines are still running:");
                    for (auto& e : list) {
                        LOG_WARN("CoroTracker:    ", e.self->name());
                    }
                }

                state = done;
            }
        });
    }

    ~GlobalState() {
        {
            lock_guard guard(mutex);
            _keep_going = false;
        }
        thread.join();
    }
};

CoroTracker::CoroTracker(const char* name, bool after_stop)
    : _name(name)
{
    auto& g = global_state();
    lock_guard guard(g.mutex);

    _entry.self = this;

    if (g.state >= State::stopped) {
        if (!after_stop) {
            LOG_WARN("CoroTracker: new coro started in stopped process");
            LOG_WARN("CoroTracker:    ", name);
        } else {
            LOG_DEBUG("CoroTracker: new coroutine started: ", name);
        }
    }

    g.list.push_back(_entry);
}

CoroTracker::~CoroTracker() {
    auto& g = global_state();
    lock_guard guard(g.mutex);

    if (g.state >= State::stopped) {
        if (g.state == State::stopped) {
            LOG_DEBUG("CoroTracker: stopped ", _name);
        } else {
            LOG_INFO("CoroTracker: stopped ", _name);
        }
    }
}

/* static */
void CoroTracker::stopped()
{
    global_state().stop();
}

/* static */
CoroTracker::GlobalState& CoroTracker::global_state() {
    static GlobalState s;
    return s;
}

} // ouinet namespace
