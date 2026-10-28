#ifndef KRONO_SYNCH_SYNCH_GPIO_H_
#define KRONO_SYNCH_SYNCH_GPIO_H_

#include "synch/Synch.h"

// ESP-IDF only: the wire for krono synch's interrupt mode. The client's pulse
// pin is wired to the server's capture pin (and the grounds joined). The wire
// is active low and both ends leave it Hi-Z: the client's pin is open drain,
// released between pulses; the server's is an input with the pull-up.
namespace krono
{

// Client: gpio as an open-drain output, released (Hi-Z). The returned pulse
// pulls it low for width_us and returns the falling edge in monotonic µs
// (esp_timer). Empty
// for a negative gpio or one that cannot be configured.
Clock make_gpio_pulse(int gpio, uint32_t width_us = 20);

// Server: gpio as an input with pull-up, falling-edge interrupt, stamped with esp_timer in the ISR.
// The returned source takes the latest pending edge. Empty when the pin or
// the ISR cannot be set up (or gpio is negative). One capture pin per
// program.
EdgeSource start_gpio_edge_capture(int gpio);

}  // namespace krono

#endif  // KRONO_SYNCH_SYNCH_GPIO_H_
