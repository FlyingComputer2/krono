#include "synch/Synch.h"

#include <atomic>
#include <deque>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace krono;

namespace
{

// One direction of an in-memory datagram link.
class Queue
{
public:
    bool push(const uint8_t* data, size_t len)
    {
        std::lock_guard<std::mutex> lock(mu_);
        q_.emplace_back(data, data + len);
        return true;
    }
    bool pop(std::vector<uint8_t>* out = nullptr)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (q_.empty())
        {
            return false;
        }
        if (out)
        {
            *out = std::move(q_.front());
        }
        q_.pop_front();
        return true;
    }

private:
    std::mutex mu_;
    std::deque<std::vector<uint8_t>> q_;
};

// Sends into tx, receives from rx.
Transport endpoint(std::shared_ptr<Queue> tx, std::shared_ptr<Queue> rx)
{
    return Transport{[tx](const uint8_t* d, size_t n) { return tx->push(d, n); },
                     [rx](std::vector<uint8_t>* out) { return rx->pop(out); }};
}

// Client and server upstreams joined back to back: what one sends, the other
// receives. A thread runs the server's poller; the clocks are fakes.
class SynchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        c2s = std::make_shared<Queue>();
        s2c = std::make_shared<Queue>();
        client_up = endpoint(c2s, s2c);
        server_up = endpoint(s2c, c2s);
    }
    void TearDown() override
    {
        stop_server.store(true);
        if (poller.joinable())
        {
            poller.join();
        }
    }
    void start_server(EdgeSource edges = {})
    {
        // Server reference = its monotonic counter + 5 s.
        server = std::make_unique<SynchServer>(
            server_up, std::move(edges), [this] { return mono.load() + 5000000; },
            [this] { return mono.load(); });
        poller = std::thread(
            [this]
            {
                while (!stop_server.load())
                {
                    server->poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
    }
    ClientOptions options()
    {
        ClientOptions o;
        o.timeout_ms = 200;
        // Client monotonic = shared counter; its local clock starts 1 s late.
        o.monotonic = [this] { return mono.load(); };
        o.local = [this] { return mono.load() + local_offset.load(); };
        o.step = [this](int64_t d)
        {
            local_offset.fetch_add(d);
            ++steps;
            return true;
        };
        o.slew = [this](int64_t d)
        {
            last_slew = d;
            return true;
        };
        return o;
    }
    // Advances the fake monotonic clock while the client waits.
    std::thread ticker()
    {
        return std::thread(
            [this]
            {
                while (!stop_ticker.load())
                {
                    mono.fetch_add(100);
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
    }

    std::shared_ptr<Queue> c2s, s2c;
    Transport client_up, server_up;
    std::unique_ptr<SynchServer> server;
    std::thread poller;
    std::atomic<bool> stop_server{false};
    std::atomic<bool> stop_ticker{false};
    std::atomic<int64_t> mono{1000000};
    std::atomic<int64_t> local_offset{1000000};
    std::atomic<int64_t> last_slew{0};
    std::atomic<int> steps{0};
};

}  // namespace

TEST(SynchWire, RoundTrips)
{
    uint8_t buf[k_response_bytes];
    Message m;
    m.type = MsgType::sync_interrupt_response;
    m.trid = 0x01020304;
    m.server_time_us = -1234567890123;
    ASSERT_EQ(encode(m, buf), k_response_bytes);
    EXPECT_EQ(buf[0], 4);
    EXPECT_EQ(buf[1], 0x01);
    EXPECT_EQ(buf[4], 0x04);
    Message out;
    ASSERT_TRUE(decode(buf, k_response_bytes, &out));
    EXPECT_EQ(out.type, m.type);
    EXPECT_EQ(out.trid, m.trid);
    EXPECT_EQ(out.server_time_us, m.server_time_us);

    m.type = MsgType::sync_request;
    ASSERT_EQ(encode(m, buf), k_request_bytes);
    ASSERT_TRUE(decode(buf, k_request_bytes, &out));
    EXPECT_EQ(out.type, MsgType::sync_request);
    EXPECT_EQ(out.server_time_us, 0);
}

TEST(SynchWire, RejectsBadLengthAndType)
{
    uint8_t buf[k_response_bytes] = {};
    Message out;
    buf[0] = 1;
    EXPECT_FALSE(decode(buf, k_response_bytes, &out));  // request too long
    buf[0] = 2;
    EXPECT_FALSE(decode(buf, k_request_bytes, &out));  // response too short
    buf[0] = 9;
    EXPECT_FALSE(decode(buf, k_request_bytes, &out));
}

TEST_F(SynchTest, RttStepsThenSlews)
{
    start_server();
    auto tick = ticker();
    SynchClient client(client_up, options());
    ASSERT_TRUE(client.step_once());
    ClientStats s = client.stats();
    EXPECT_TRUE(s.synced);
    EXPECT_EQ(steps.load(), 1);
    // Local was mono + 1 s; the server is mono + 5 s. Fake time moves only
    // in 100 µs steps while the reply is in flight, so within a few ms.
    EXPECT_NEAR(local_offset.load(), 5000000, 5000);
    EXPECT_GE(s.applied.delay_us, 0);

    ASSERT_TRUE(client.step_once());
    s = client.stats();
    EXPECT_EQ(steps.load(), 1);
    EXPECT_EQ(s.slews, 1u);
    EXPECT_LT(std::abs(last_slew.load()), 5000);
    stop_ticker.store(true);
    tick.join();
}

TEST_F(SynchTest, InterruptUsesEdgeTime)
{
    // The pulse at monotonic T0 raises an edge the server stamps at T0 too
    // (one shared fake counter).
    std::atomic<bool> pending{false};
    std::atomic<int64_t> edge{0};
    start_server(
        [&](int64_t* out)
        {
            *out = edge.load();
            return pending.exchange(false);
        });
    auto tick = ticker();
    ClientOptions o = options();
    o.mode = Mode::interrupt;
    o.pulse = [&]
    {
        const int64_t t0 = mono.load();
        edge.store(t0);
        pending.store(true);
        return t0;
    };
    SynchClient client(client_up, o);
    ASSERT_TRUE(client.step_once());
    // offset = (T0 + 5 s) - T0 whatever the air delay; the ticker may
    // advance between the server's two clock reads.
    EXPECT_NEAR(client.stats().applied.offset_us, 5000000, 500);
    stop_ticker.store(true);
    tick.join();
}

TEST_F(SynchTest, InterruptWithoutEdgeTimesOut)
{
    start_server([](int64_t*) { return false; });
    auto tick = ticker();
    ClientOptions o = options();
    o.mode = Mode::interrupt;
    o.timeout_ms = 20;
    o.pulse = [this] { return mono.load(); };
    SynchClient client(client_up, o);
    EXPECT_FALSE(client.step_once());
    EXPECT_EQ(client.stats().timeouts, 1u);
    EXPECT_EQ(steps.load(), 0);
    for (int i = 0; i < 200 && server->stats().no_edge == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(server->stats().no_edge, 1u);
    stop_ticker.store(true);
    tick.join();
}

TEST_F(SynchTest, RttKeepsLowestDelaySample)
{
    // No server: replies are injected by hand into the client's receive
    // queue, so each exchange's delay is under test control.
    ClientOptions o = options();
    // Above the slow reply's 82 ms: the fake clock jumps by the delay
    // before the reply is queued.
    o.timeout_ms = 1000;
    o.apply_clock = false;
    SynchClient client(client_up, o);
    auto reply = [&](uint32_t trid, int64_t server_time, int64_t delay)
    {
        // Answer the request just sent: advance the clock by the delay.
        std::thread t(
            [&, trid, server_time, delay]
            {
                while (!c2s->pop())
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                mono.fetch_add(delay);
                Message m;
                m.type = MsgType::sync_response;
                m.trid = trid;
                m.server_time_us = server_time;
                uint8_t buf[k_response_bytes];
                server_up.send(buf, encode(m, buf));
            });
        const bool ok = client.step_once();
        t.join();
        return ok;
    };
    // Fast sample: T1 = 1 s, T4 = 1.002 s, S = 6.001 s -> offset 5 s.
    ASSERT_TRUE(reply(1, 6001000, 2000));
    EXPECT_EQ(client.stats().applied.offset_us, 5000000);
    // Slow, one-sided sample (offset would be wrong by 40 ms); not applied.
    const int64_t t1 = mono.load();
    ASSERT_TRUE(reply(2, t1 + 5000000 + 1000, 82000));
    EXPECT_EQ(client.stats().applied.delay_us, 2000);
    EXPECT_EQ(client.stats().applied.offset_us, 5000000);
}

TEST_F(SynchTest, ConsoleModeAndStatus)
{
    SynchClient client(client_up, options());
    EXPECT_EQ(client.handle_command("mode interrupt\n"), "error no pulse pin\n");
    EXPECT_EQ(client.handle_command("mode monitor\n"), "error no pulse pin\n");
    EXPECT_EQ(client.handle_command("mode bogus\n"),
              "error usage: mode <rtt|interrupt|monitor>\n");
    EXPECT_NE(client.handle_command("status\n").find("mode rtt\n"),
              std::string::npos);
}

TEST_F(SynchTest, MonitorGradesRttOverTheWire)
{
    std::atomic<bool> pending{false};
    std::atomic<int64_t> edge{0};
    start_server(
        [&](int64_t* out)
        {
            *out = edge.load();
            return pending.exchange(false);
        });
    auto tick = ticker();
    ClientOptions o = options();
    o.mode = Mode::monitor;
    o.pulse = [&]
    {
        const int64_t t0 = mono.load();
        edge.store(t0);
        pending.store(true);
        return t0;
    };
    SynchClient client(client_up, o);
    // First period: the wire reads the unsynced error (local mono + 1 s vs
    // server mono + 5 s), then rtt steps.
    ASSERT_TRUE(client.step_once());
    ClientStats s = client.stats();
    EXPECT_EQ(s.wire_samples, 1u);
    // The ticker may advance between clock reads.
    EXPECT_NEAR(s.wire_last_us, -4000000, 500);
    EXPECT_EQ(steps.load(), 1);
    EXPECT_EQ(s.applied.mode, Mode::rtt);
    // Second period: the wire grades what rtt left, within its fake delay.
    ASSERT_TRUE(client.step_once());
    s = client.stats();
    EXPECT_EQ(s.wire_samples, 2u);
    EXPECT_LT(std::abs(s.wire_last_us), 5000);
    EXPECT_NE(client.handle_command("status\n").find("wire_samples 2\n"),
              std::string::npos);
    EXPECT_EQ(client.handle_command("clr\n"), "ok\n");
    EXPECT_EQ(client.stats().wire_samples, 0u);
    stop_ticker.store(true);
    tick.join();
}

TEST(SynchServerAnswer, AnswersWithoutATransport)
{
    SynchServer server({}, {}, [] { return int64_t(42); }, [] { return int64_t(0); });
    uint8_t req[k_request_bytes];
    Message m;
    m.type = MsgType::sync_request;
    m.trid = 7;
    ASSERT_EQ(encode(m, req), k_request_bytes);
    uint8_t out[k_response_bytes];
    ASSERT_EQ(server.answer(req, sizeof(req), out), k_response_bytes);
    Message reply;
    ASSERT_TRUE(decode(out, k_response_bytes, &reply));
    EXPECT_EQ(reply.type, MsgType::sync_response);
    EXPECT_EQ(reply.trid, 7u);
    EXPECT_EQ(reply.server_time_us, 42);
    EXPECT_EQ(server.stats().replies, 1u);
    // No edge source: an interrupt request has no reply.
    m.type = MsgType::sync_interrupt_request;
    encode(m, req);
    EXPECT_EQ(server.answer(req, sizeof(req), out), 0u);
    EXPECT_EQ(server.stats().no_edge, 1u);
    EXPECT_EQ(server.answer(out, 3, out), 0u);
    EXPECT_EQ(server.stats().bad, 1u);
}
