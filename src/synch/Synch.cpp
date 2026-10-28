#include "synch/Synch.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

namespace krono
{

namespace
{

std::atomic<bool> g_stop{false};

// Sleep between receive polls. The transport has no blocking receive, so a
// reply waits up to one tick (1 ms on the P4) on each side.
constexpr auto k_tick = std::chrono::milliseconds(1);

void put_be(uint8_t* out, uint64_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * (bytes - 1 - i)));
    }
}

uint64_t get_be(const uint8_t* in, size_t bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
    {
        value = (value << 8) | in[i];
    }
    return value;
}

bool send_message(const Transport& transport, const Message& msg)
{
    uint8_t buf[k_response_bytes];
    return transport.send && transport.send(buf, encode(msg, buf));
}

// "host:port", host an IPv4 address (0.0.0.0 for any).
bool parse_host_port(const std::string& text, sockaddr_in* out)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon + 1 == text.size())
    {
        return false;
    }
    char* end = nullptr;
    const long port = std::strtol(text.c_str() + colon + 1, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535)
    {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, text.substr(0, colon).c_str(), &addr.sin_addr) != 1)
    {
        return false;
    }
    *out = addr;
    return true;
}

timeval to_timeval(int64_t us)
{
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(us / 1000000);
    tv.tv_usec = static_cast<suseconds_t>(us % 1000000);
    if (tv.tv_usec < 0)
    {
        tv.tv_sec -= 1;
        tv.tv_usec += 1000000;
    }
    return tv;
}

bool step_realtime(int64_t delta_us)
{
    const timeval tv = to_timeval(realtime_us() + delta_us);
    return settimeofday(&tv, nullptr) == 0;
}

bool slew_realtime(int64_t delta_us)
{
    const timeval tv = to_timeval(delta_us);
    return adjtime(&tv, nullptr) == 0;
}

int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

std::string first_word(std::istringstream& in)
{
    std::string word;
    in >> word;
    return word;
}

// Binds the UDP console; -1 when the address is bad or taken.
int open_console(const std::string& console)
{
    sockaddr_in addr{};
    if (!parse_host_port(console, &addr))
    {
        fprintf(stderr, "synch: bad console address %s\n", console.c_str());
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    // Wakes the loop to check stop().
    timeval timeout{};
    timeout.tv_usec = 100 * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        fprintf(stderr, "synch: cannot bind %s\n", console.c_str());
        close(fd);
        return -1;
    }
    return fd;
}

void run_console(int fd, const std::function<std::string(const std::string&)>& handle)
{
    char buf[256];
    while (!g_stop.load())
    {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0)
        {
            continue;
        }
        buf[n] = '\0';
        const std::string reply = handle(buf);
        sendto(fd, reply.data(), reply.size(), 0,
               reinterpret_cast<const sockaddr*>(&from), from_len);
    }
}

}  // namespace

bool is_response(MsgType type)
{
    return type == MsgType::sync_response ||
           type == MsgType::sync_interrupt_response;
}

size_t encode(const Message& msg, uint8_t* out)
{
    out[0] = static_cast<uint8_t>(msg.type);
    put_be(out + 1, msg.trid, 4);
    if (!is_response(msg.type))
    {
        return k_request_bytes;
    }
    put_be(out + 5, static_cast<uint64_t>(msg.server_time_us), 8);
    return k_response_bytes;
}

bool decode(const uint8_t* data, size_t len, Message* out)
{
    if (!data || len < k_request_bytes)
    {
        return false;
    }
    const auto type = static_cast<MsgType>(data[0]);
    switch (type)
    {
    case MsgType::sync_request:
    case MsgType::sync_interrupt_request:
        if (len != k_request_bytes)
        {
            return false;
        }
        break;
    case MsgType::sync_response:
    case MsgType::sync_interrupt_response:
        if (len != k_response_bytes)
        {
            return false;
        }
        break;
    default:
        return false;
    }
    out->type = type;
    out->trid = static_cast<uint32_t>(get_be(data + 1, 4));
    out->server_time_us =
        is_response(type) ? static_cast<int64_t>(get_be(data + 5, 8)) : 0;
    return true;
}

int64_t realtime_us()
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

int64_t monotonic_us()
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

SynchServer::SynchServer(Transport transport, EdgeSource edges, Clock reference,
                         Clock monotonic)
    : transport_(std::move(transport)),
      edges_(std::move(edges)),
      reference_(std::move(reference)),
      monotonic_(std::move(monotonic))
{
}

size_t SynchServer::poll()
{
    size_t count = 0;
    std::vector<uint8_t> packet;
    while (transport_.receive && transport_.receive(&packet))
    {
        ++count;
        uint8_t reply[k_response_bytes];
        const size_t n = answer(packet.data(), packet.size(), reply);
        if (n && transport_.send)
        {
            transport_.send(reply, n);
        }
    }
    return count;
}

size_t SynchServer::answer(const uint8_t* data, size_t len, uint8_t* out)
{
    Message msg;
    if (!decode(data, len, &msg) || is_response(msg.type))
    {
        bad_.fetch_add(1);
        return 0;
    }
    Message reply;
    reply.trid = msg.trid;
    if (msg.type == MsgType::sync_request)
    {
        requests_.fetch_add(1);
        reply.type = MsgType::sync_response;
        reply.server_time_us = reference_();
        replies_.fetch_add(1);
        return encode(reply, out);
    }
    interrupt_requests_.fetch_add(1);
    int64_t edge = 0;
    // The ISR can only stamp the monotonic counter; carry the edge onto the
    // reference clock by its age. krono never writes the server clock; an
    // SNTP slew in progress bends the age by at most 1/64.
    const int64_t mono_now = monotonic_();
    if (!edges_ || !edges_(&edge) || mono_now - edge > edge_max_age_us)
    {
        no_edge_.fetch_add(1);
        return 0;
    }
    reply.type = MsgType::sync_interrupt_response;
    reply.server_time_us = reference_() - (mono_now - edge);
    replies_.fetch_add(1);
    return encode(reply, out);
}

ServerStats SynchServer::stats() const
{
    ServerStats s;
    s.requests = requests_.load();
    s.interrupt_requests = interrupt_requests_.load();
    s.replies = replies_.load();
    s.no_edge = no_edge_.load();
    s.bad = bad_.load();
    return s;
}

std::string SynchServer::handle_command(const std::string& line)
{
    std::istringstream in(line);
    if (first_word(in) == "status")
    {
        const ServerStats s = stats();
        char out[192];
        snprintf(out, sizeof(out),
                 "role server\ntime_us %lld\nrequests %llu\n"
                 "interrupt_requests %llu\nreplies %llu\nno_edge %llu\nbad %llu\n",
                 static_cast<long long>(reference_()),
                 static_cast<unsigned long long>(s.requests),
                 static_cast<unsigned long long>(s.interrupt_requests),
                 static_cast<unsigned long long>(s.replies),
                 static_cast<unsigned long long>(s.no_edge),
                 static_cast<unsigned long long>(s.bad));
        return out;
    }
    return "error unknown command (status)\n";
}

const char* mode_name(Mode mode)
{
    switch (mode)
    {
    case Mode::interrupt:
        return "interrupt";
    case Mode::monitor:
        return "monitor";
    default:
        return "rtt";
    }
}

bool parse_mode(const std::string& text, Mode* out)
{
    if (text == "rtt")
    {
        *out = Mode::rtt;
        return true;
    }
    if (text == "interrupt")
    {
        *out = Mode::interrupt;
        return true;
    }
    if (text == "monitor")
    {
        *out = Mode::monitor;
        return true;
    }
    return false;
}

SynchClient::SynchClient(Transport transport, ClientOptions options)
    : transport_(std::move(transport)),
      options_(std::move(options)),
      mode_(options_.mode)
{
    if (!options_.step)
    {
        options_.step = step_realtime;
    }
    if (!options_.slew)
    {
        options_.slew = slew_realtime;
    }
    options_.window = std::max<size_t>(options_.window, 1);
}

std::optional<Sample> SynchClient::exchange(Mode measurement)
{
    const Mode mode = measurement == Mode::interrupt ? Mode::interrupt : Mode::rtt;
    // Replies to earlier, timed-out requests are stale from here on.
    std::vector<uint8_t> packet;
    while (transport_.receive && transport_.receive(&packet))
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.stale;
    }
    Message req;
    req.trid = next_trid_++;
    int64_t t1 = 0;
    if (mode == Mode::rtt)
    {
        req.type = MsgType::sync_request;
        t1 = options_.monotonic();
    }
    else
    {
        if (!options_.pulse)
        {
            return std::nullopt;
        }
        req.type = MsgType::sync_interrupt_request;
        t1 = options_.pulse();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.requests;
    }
    if (!send_message(transport_, req))
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.timeouts;
        return std::nullopt;
    }
    const MsgType want = mode == Mode::rtt ? MsgType::sync_response
                                           : MsgType::sync_interrupt_response;
    const int64_t deadline =
        options_.monotonic() + int64_t(options_.timeout_ms) * 1000;
    for (;;)
    {
        while (transport_.receive(&packet))
        {
            const int64_t t4 = options_.monotonic();
            Message msg;
            if (!decode(packet.data(), packet.size(), &msg) ||
                msg.type != want || msg.trid != req.trid)
            {
                std::lock_guard<std::mutex> lock(mu_);
                ++stats_.stale;
                continue;
            }
            Sample s;
            s.mode = mode;
            s.at_us = t4;
            s.delay_us = t4 - t1;
            s.offset_us = mode == Mode::rtt
                              ? msg.server_time_us - (t1 + s.delay_us / 2)
                              : msg.server_time_us - t1;
            std::lock_guard<std::mutex> lock(mu_);
            ++stats_.replies;
            return s;
        }
        if (options_.monotonic() >= deadline || g_stop.load())
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++stats_.timeouts;
            return std::nullopt;
        }
        std::this_thread::sleep_for(k_tick);
    }
}

bool SynchClient::step_once()
{
    const Mode mode = mode_.load();
    if (mode == Mode::monitor)
    {
        // Before the correction, so the reading sees the clock as the last
        // rtt correction left it, with its slew finished.
        record_wire(exchange(Mode::interrupt));
    }
    const auto sample = exchange(mode);
    if (!sample)
    {
        return false;
    }
    apply(*sample);
    return true;
}

void SynchClient::apply(const Sample& sample)
{
    Sample best;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // A mode change starts a new window: the two measure different paths.
        if (!window_.empty() && window_.back().mode != sample.mode)
        {
            window_.clear();
        }
        window_.push_back(sample);
        while (window_.size() > options_.window)
        {
            window_.pop_front();
        }
        best = sample;
        if (sample.mode == Mode::rtt)
        {
            best = *std::min_element(window_.begin(), window_.end(),
                                     [](const Sample& a, const Sample& b)
                                     { return a.delay_us < b.delay_us; });
        }
        stats_.applied = best;
    }
    // Server time now, by the chosen sample's offset, against the local clock.
    const int64_t error = options_.local() - (options_.monotonic() + best.offset_us);
    std::lock_guard<std::mutex> lock(mu_);
    stats_.error_us = error;
    if (!options_.apply_clock)
    {
        return;
    }
    if (!stats_.synced || abs64(error) > options_.step_us)
    {
        if (options_.step(-error))
        {
            ++stats_.steps;
            stats_.synced = true;
            printf("synch: stepped %lld us (%s, delay %lld us)\n",
                   static_cast<long long>(-error), mode_name(best.mode),
                   static_cast<long long>(best.delay_us));
        }
    }
    else if (options_.slew(-error))
    {
        ++stats_.slews;
    }
}

void SynchClient::record_wire(const std::optional<Sample>& sample)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!sample)
    {
        ++stats_.wire_timeouts;
        return;
    }
    const int64_t error = options_.local() - (options_.monotonic() + sample->offset_us);
    if (stats_.wire_samples == 0 || error < stats_.wire_min_us)
    {
        stats_.wire_min_us = error;
    }
    if (stats_.wire_samples == 0 || error > stats_.wire_max_us)
    {
        stats_.wire_max_us = error;
    }
    ++stats_.wire_samples;
    stats_.wire_last_us = error;
    stats_.wire_sum_us += error;
    stats_.wire_sum_sq += double(error) * double(error);
}

void SynchClient::clear_wire()
{
    std::lock_guard<std::mutex> lock(mu_);
    stats_.wire_samples = 0;
    stats_.wire_timeouts = 0;
    stats_.wire_last_us = 0;
    stats_.wire_min_us = 0;
    stats_.wire_max_us = 0;
    stats_.wire_sum_us = 0;
    stats_.wire_sum_sq = 0;
}

void SynchClient::set_mode(Mode mode)
{
    if (mode == Mode::monitor && mode_.load() != Mode::monitor)
    {
        clear_wire();
    }
    mode_.store(mode);
}

Mode SynchClient::mode() const
{
    return mode_.load();
}

ClientStats SynchClient::stats() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

std::string SynchClient::handle_command(const std::string& line)
{
    std::istringstream in(line);
    const std::string cmd = first_word(in);
    if (cmd == "status")
    {
        const ClientStats s = stats();
        char out[384];
        snprintf(out, sizeof(out),
                 "role client\nmode %s\nsynced %d\nerror_us %lld\noffset_us %lld\n"
                 "delay_us %lld\nrequests %llu\nreplies %llu\ntimeouts %llu\n"
                 "stale %llu\nsteps %llu\nslews %llu\n",
                 mode_name(mode()), s.synced ? 1 : 0,
                 static_cast<long long>(s.error_us),
                 static_cast<long long>(s.applied.offset_us),
                 static_cast<long long>(s.applied.delay_us),
                 static_cast<unsigned long long>(s.requests),
                 static_cast<unsigned long long>(s.replies),
                 static_cast<unsigned long long>(s.timeouts),
                 static_cast<unsigned long long>(s.stale),
                 static_cast<unsigned long long>(s.steps),
                 static_cast<unsigned long long>(s.slews));
        std::string reply = out;
        if (mode() == Mode::monitor)
        {
            const double n = double(s.wire_samples);
            const double mean = n ? double(s.wire_sum_us) / n : 0;
            const double rms = n ? std::sqrt(s.wire_sum_sq / n) : 0;
            snprintf(out, sizeof(out),
                     "wire_samples %llu\nwire_timeouts %llu\nwire_last_us %lld\n"
                     "wire_mean_us %.0f\nwire_rms_us %.0f\nwire_min_us %lld\n"
                     "wire_max_us %lld\n",
                     static_cast<unsigned long long>(s.wire_samples),
                     static_cast<unsigned long long>(s.wire_timeouts),
                     static_cast<long long>(s.wire_last_us), mean, rms,
                     static_cast<long long>(s.wire_min_us),
                     static_cast<long long>(s.wire_max_us));
            reply += out;
        }
        return reply;
    }
    if (cmd == "clr")
    {
        clear_wire();
        return "ok\n";
    }
    if (cmd == "mode")
    {
        Mode mode;
        std::string extra;
        if (!parse_mode(first_word(in), &mode) || (in >> extra))
        {
            return "error usage: mode <rtt|interrupt|monitor>\n";
        }
        if (mode != Mode::rtt && !options_.pulse)
        {
            return "error no pulse pin\n";
        }
        set_mode(mode);
        return "ok\n";
    }
    return "error unknown command (status, mode, clr)\n";
}

int server_main(const ServerConfig& config)
{
    if (!config.transport.send || !config.transport.receive)
    {
        fprintf(stderr, "synch: server_main needs a transport\n");
        return 1;
    }
    const int fd = open_console(config.console);
    if (fd < 0)
    {
        return 1;
    }
    g_stop.store(false);
    SynchServer server(config.transport, config.edges);
    std::thread poller(
        [&server]
        {
            while (!g_stop.load())
            {
                if (server.poll() == 0)
                {
                    std::this_thread::sleep_for(k_tick);
                }
            }
        });
    printf("synch: server console %s, pulse capture %s\n", config.console.c_str(),
           config.edges ? "on" : "off");
    run_console(fd, [&server](const std::string& line)
                { return server.handle_command(line); });
    poller.join();
    close(fd);
    return 0;
}

int client_main(const ClientConfig& config)
{
    if (!config.transport.send || !config.transport.receive || config.period_ms == 0)
    {
        fprintf(stderr, "synch: client_main needs a transport and a period\n");
        return 1;
    }
    const int fd = open_console(config.console);
    if (fd < 0)
    {
        return 1;
    }
    g_stop.store(false);
    SynchClient client(config.transport, config.options);
    std::thread worker(
        [&client, period = std::chrono::milliseconds(config.period_ms)]
        {
            auto next = std::chrono::steady_clock::now();
            while (!g_stop.load())
            {
                client.step_once();
                next += period;
                const auto now = std::chrono::steady_clock::now();
                if (next < now)
                {
                    next = now;
                }
                while (!g_stop.load() && std::chrono::steady_clock::now() < next)
                {
                    std::this_thread::sleep_for(
                        std::min<std::chrono::steady_clock::duration>(
                            next - std::chrono::steady_clock::now(),
                            std::chrono::milliseconds(100)));
                }
            }
        });
    printf("synch: client console %s, mode %s, period %u ms\n",
           config.console.c_str(), mode_name(config.options.mode),
           static_cast<unsigned>(config.period_ms));
    run_console(fd, [&client](const std::string& line)
                { return client.handle_command(line); });
    worker.join();
    close(fd);
    return 0;
}

void stop()
{
    g_stop.store(true);
}

}  // namespace krono
