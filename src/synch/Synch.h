#ifndef KRONO_SYNCH_SYNCH_H_
#define KRONO_SYNCH_SYNCH_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Clock synchronization over any datagram transport (an L3 upstream queue, a
// USB channel, a socket). SynchServer holds the reference time and only
// answers; SynchClient measures its offset to the server and steers its own
// system clock. Two measurements:
//   rtt:       client sends sync_request{trid} at T1, the server answers
//              sync_response{trid, server_time_us} (T2 = T3), the client takes
//              T4 on arrival: offset = S - (T1 + T4) / 2, delay = T4 - T1.
//   interrupt: client pulls a wire to the server's capture pin low at T0
//              (active low, Hi-Z at rest), then sends
//              sync_interrupt_request{trid}; the server answers
//              sync_interrupt_response{trid, server_time_us} with the time of
//              that edge: offset = S - T0, independent of the air.
// Wire (big-endian): [u8 type][u32 trid], responses add [i64 server_time_us].
namespace krono
{

// One datagram per call, nonblocking. send returns false when the datagram
// was not taken; receive returns false when nothing is queued.
struct Transport
{
    std::function<bool(const uint8_t* data, size_t len)> send;
    std::function<bool(std::vector<uint8_t>* out)> receive;
};

enum class MsgType : uint8_t
{
    sync_request = 1,
    sync_response = 2,
    sync_interrupt_request = 3,
    sync_interrupt_response = 4,
};

struct Message
{
    MsgType type = MsgType::sync_request;
    uint32_t trid = 0;
    // Responses only.
    int64_t server_time_us = 0;
};

constexpr size_t k_request_bytes = 5;
constexpr size_t k_response_bytes = 13;

bool is_response(MsgType type);
// Writes the message to out (k_response_bytes at most); returns its length.
size_t encode(const Message& msg, uint8_t* out);
// False for an unknown type or a length that does not match it.
bool decode(const uint8_t* data, size_t len, Message* out);

using Clock = std::function<int64_t()>;
// CLOCK_REALTIME in µs: the time the server hands out and the client steers.
int64_t realtime_us();
// Free-running µs counter; on ESP-IDF esp_timer_get_time(), the same counter
// the GPIO glue stamps edges and pulses with.
int64_t monotonic_us();

// Takes the pending capture edge (monotonic µs) and clears it; false when
// none is pending.
using EdgeSource = std::function<bool(int64_t* monotonic_us)>;

struct ServerStats
{
    uint64_t requests = 0;
    uint64_t interrupt_requests = 0;
    uint64_t replies = 0;
    // Interrupt requests left unanswered: no edge, or one older than
    // edge_max_age_us.
    uint64_t no_edge = 0;
    uint64_t bad = 0;
};

class SynchServer
{
public:
    // edges may be empty: interrupt requests then go unanswered. transport
    // may be empty for a server fed only through answer().
    SynchServer(Transport transport,
                EdgeSource edges = {}, Clock reference = realtime_us,
                Clock monotonic = monotonic_us);
    SynchServer(const SynchServer&) = delete;
    SynchServer& operator=(const SynchServer&) = delete;

    // Answers every queued request; returns datagrams taken.
    size_t poll();
    // One request in, its reply written to out (k_response_bytes); returns
    // the reply length, 0 when there is none (bad datagram, no edge). The
    // reference time is read here, so call it as the request arrives.
    size_t answer(const uint8_t* data, size_t len, uint8_t* out);
    ServerStats stats() const;
    std::string handle_command(const std::string& line);

    // An edge older than this when its request arrives belongs to an earlier
    // pulse (its request was lost); it is not handed out.
    int64_t edge_max_age_us = 500 * 1000;

private:
    Transport transport_;
    EdgeSource edges_;
    Clock reference_;
    Clock monotonic_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> interrupt_requests_{0};
    std::atomic<uint64_t> replies_{0};
    std::atomic<uint64_t> no_edge_{0};
    std::atomic<uint64_t> bad_{0};
};

enum class Mode
{
    rtt,
    interrupt,
    // Steers by rtt and grades it: each period first measures the clock
    // error over the wire (interrupt exchange, not applied), then corrects by
    // rtt. Needs a pulse pin.
    monitor,
};

const char* mode_name(Mode mode);
bool parse_mode(const std::string& text, Mode* out);

struct Sample
{
    Mode mode = Mode::rtt;
    // Server reference time minus client monotonic time.
    int64_t offset_us = 0;
    // rtt: T4 - T1. interrupt: pulse to reply, for information only.
    int64_t delay_us = 0;
    // Client monotonic time when the reply arrived.
    int64_t at_us = 0;
};

struct ClientOptions
{
    Mode mode = Mode::rtt;
    // Reply wait per exchange.
    uint32_t timeout_ms = 300;
    // rtt: the applied sample is the lowest-delay one of the last `window`
    // (queueing only ever adds delay). interrupt: the latest one is applied.
    size_t window = 8;
    // Correct the system clock (step / slew); off = measure only.
    bool apply_clock = true;
    // A clock error larger than this is stepped, a smaller one slewed. The
    // first applied sample always steps.
    int64_t step_us = 20 * 1000;
    // Interrupt mode: pulses the wire, returns its asserting edge in
    // monotonic µs. Without it interrupt exchanges fail.
    Clock pulse;
    Clock monotonic = monotonic_us;
    // The clock being steered, and how. Defaults: CLOCK_REALTIME,
    // settimeofday (step) and adjtime (slew).
    Clock local = realtime_us;
    std::function<bool(int64_t delta_us)> step;
    std::function<bool(int64_t delta_us)> slew;
};

struct ClientStats
{
    uint64_t requests = 0;
    uint64_t replies = 0;
    uint64_t timeouts = 0;
    // Replies that matched no outstanding request (late or duplicate).
    uint64_t stale = 0;
    uint64_t steps = 0;
    uint64_t slews = 0;
    bool synced = false;
    // Last applied sample.
    Sample applied;
    // Local clock minus the server's, as of the last correction.
    int64_t error_us = 0;
    // Monitor mode: the clock error read over the wire before each rtt
    // correction, since the mode was entered or `clr`.
    uint64_t wire_samples = 0;
    uint64_t wire_timeouts = 0;
    int64_t wire_last_us = 0;
    int64_t wire_min_us = 0;
    int64_t wire_max_us = 0;
    int64_t wire_sum_us = 0;
    double wire_sum_sq = 0;
};

class SynchClient
{
public:
    SynchClient(Transport transport, ClientOptions options);
    SynchClient(const SynchClient&) = delete;
    SynchClient& operator=(const SynchClient&) = delete;

    // One exchange of the given measurement (rtt or interrupt); empty on
    // timeout or a failed pulse.
    std::optional<Sample> exchange(Mode measurement);
    // The current mode's exchange(s), then filter and correct the clock.
    // False without a sample to correct by.
    bool step_once();
    // Zeroes the monitor-mode wire statistics.
    void clear_wire();
    void set_mode(Mode mode);
    Mode mode() const;
    ClientStats stats() const;
    std::string handle_command(const std::string& line);

private:
    void apply(const Sample& sample);
    void record_wire(const std::optional<Sample>& sample);

    Transport transport_;
    ClientOptions options_;
    std::atomic<Mode> mode_;
    uint32_t next_trid_ = 1;
    mutable std::mutex mu_;
    std::deque<Sample> window_;
    ClientStats stats_;
};

struct ServerConfig
{
    Transport transport;
    EdgeSource edges;
    // UDP console, one command per datagram, reply to the sender.
    std::string console = "0.0.0.0:5110";
};

struct ClientConfig
{
    Transport transport;
    ClientOptions options;
    uint32_t period_ms = 1000;
    std::string console = "0.0.0.0:5110";
};

// Run the console and the server's poller / the client's exchange loop until
// stop(). Block; return 0, or 1 when the config or console socket is
// unusable. Threads they start inherit the caller's thread settings.
int server_main(const ServerConfig& config);
int client_main(const ClientConfig& config);

// Asks a running server_main() / client_main() to return. Safe from any
// thread.
void stop();

}  // namespace krono

#endif  // KRONO_SYNCH_SYNCH_H_
