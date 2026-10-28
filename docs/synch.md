# synch: clock sync between two hosts

`krono::SynchServer` and `krono::SynchClient` ([`src/synch/Synch.h`](../src/synch/Synch.h),
ESP-IDF component [`idf/krono`](../idf/krono)) synchronize two hosts' clocks
over any datagram transport (`krono::Transport`: nonblocking send and
receive, one datagram per call). On the rover and ground P4s, the transport
is a winject-l3 L3-owned upstream queue, wrapped by the host app. On the
ground, the Android app's USB channel 3 is fed through `SynchServer::answer()`.
The server holds the reference time (`CLOCK_REALTIME`) and only answers. The
client measures its offset to the server and steers its own
`CLOCK_REALTIME`. krono never writes the server's clock.

## Measurements

**rtt** (default):

1. The client stamps T1 and sends `sync_request{trid}`.
2. The server replies `sync_response{trid, server_time_us}` (T2 = T3).
3. The client stamps T4 when the reply arrives.

offset = S − (T1 + T4) / 2, delay = T4 − T1. Queueing only adds delay, so the
client applies the lowest-delay sample of the last `window` (8). Asymmetric
queueing still biases the offset by half of the one-sided excess.

**interrupt**: the client's pulse pin is wired to the server's capture pin,
with grounds joined. The wire is active low and Hi-Z at rest. The client's
pin is open drain and is released between pulses. The server's pin is an
input and holds the wire's only pull-up.

1. The client pulls the pulse pin low and stamps T0.
2. The server's falling-edge ISR stamps the edge.
3. The client sends `sync_interrupt_request{trid}`.
4. The server replies `sync_interrupt_response{trid, server_time_us}`, where `server_time_us` is the edge time on the reference clock.

offset = S − T0. The air delay drops out; what is left is pin and ISR
latency (microseconds). The ISR can only read the monotonic counter
(esp_timer). The server carries the edge onto the reference clock by its age
when the request arrives. An edge older than `edge_max_age_us` (500 ms) is
taken to belong to an earlier pulse whose request was lost. The server does
not reply then, and neither does it when no edge is pending. The client
counts both cases as timeouts.

**monitor**: steers by rtt and grades it over the wire. Each period starts with
an interrupt exchange that measures the clock error but does not correct it.
That reading comes before the rtt correction, so it sees the clock as the
previous correction left it, with the slew finished. The rtt exchange and
correction follow. `status` adds `wire_samples`, `wire_timeouts`,
`wire_last_us`, `wire_mean_us`, `wire_rms_us`, `wire_min_us` and
`wire_max_us`: local clock minus server clock, read over the wire, since the
mode was entered or since `clr`. Pin and ISR latency, a few µs, stays in that
reading.

Both modes keep the offset against the client's monotonic counter, so a slew
in progress does not bend the measurement.

## Correction

The first applied sample steps the client clock (`settimeofday`), and so does
any error over `step_us` (20 ms). A smaller error is slewed with `adjtime`.
On ESP-IDF a slew runs at 1/64, so 2 ms takes about 0.13 s. There is no
frequency estimate yet: at ±10 ppm per crystal the boards walk up to
20 µs/s, and the next exchange (1 s by default) slews that away.
`apply_clock = false` measures without writing the clock.

## Wire

Big-endian, one datagram per message:

| Message | Type | Body |
| --- | --- | --- |
| `sync_request` | 1 | `u32 trid` |
| `sync_response` | 2 | `u32 trid`, `i64 server_time_us` |
| `sync_interrupt_request` | 3 | `u32 trid` |
| `sync_interrupt_response` | 4 | `u32 trid`, `i64 server_time_us` |

A reply whose trid or type matches no outstanding request is counted as
`stale` and dropped.

## Console

Each side has a UDP console (default `0.0.0.0:5110`). It takes one command
per datagram and replies to the sender.

- `status`: counters, in `key value` lines. On the client these include
  `mode`, `synced`, `error_us` (local clock minus server clock at the last
  correction), `offset_us` and `delay_us` of the applied sample, `requests`,
  `replies`, `timeouts`, `stale`, `steps` and `slews`.
- `mode <rtt|interrupt|monitor>` (client only): switches the measurement.
  Interrupt and monitor need a pulse pin.
- `clr` (client only): zeroes the monitor statistics.

```sh
echo status | nc -u -w1 <client-ip> 5110
echo "mode interrupt" | nc -u -w1 <client-ip> 5110
```

## ESP-IDF glue

[`SynchGpio.h`](../src/apps/synch/SynchGpio.h) provides the pin side:

- `make_gpio_pulse(gpio)` gives the client's `pulse`. The pin is an open-drain output that idles released. A pulse pulls it low for 20 µs and returns the edge time, read inside a critical section.
- `start_gpio_edge_capture(gpio)` gives the server's `EdgeSource`. The capture pin is an input with the internal pull-up and a falling-edge IRAM ISR. An unplugged wire reads high and raises no edges.

rtt exchanges still share the radio TX queue, the SDIO link and the air with
the other upstreams. Under video they wait behind video frames, and the
fast floor on the P4 bench was 16–20 ms of round trip. Interrupt mode does
not depend on that delay.
