# krono

Clock sources and clock sync for the rover and its ground stations.

| Part | Status | |
|---|---|---|
| [synch](docs/synch.md) | in use | Two-host sync over any datagram transport: rtt exchanges, or a wire pulse into the server's capture pin. Includes a monitor mode that grades rtt against the wire. |
| GPS | planned | GNSS time and PPS as a clock source |

## Use

- **Host (Linux):** `add_subdirectory` or FetchContent this repo and link
  `krono_synch`.
- **ESP-IDF:** add `<krono>/idf/krono` to `EXTRA_COMPONENT_DIRS` and require
  `krono`. The `SynchGpio.h` pulse and edge-capture glue is ESP-IDF only.

```cpp
#include "synch/Synch.h"

krono::ClientConfig cfg;
cfg.transport = {send_fn, receive_fn};  // nonblocking, one datagram per call
krono::client_main(cfg);                // console on UDP 5110
```

## Tests

```sh
cmake -S . -B build -G Ninja -DKRONO_BUILD_TESTS=ON
cmake --build build && build/krono_tests
```

## Deployed

- The rover P4 (rov-esp32p4) runs `SynchClient` on L3 upstream 0, with the
  pulse on GPIO20.
- The ground P4 (gnd-esp32p4) runs `SynchServer` on L3 upstream 0, capturing
  on GPIO20. It also answers the Android app on USB channel 3.
- The ground follows the GS host's chrony over SNTP.
