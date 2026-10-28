#include "synch/SynchGpio.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cstdio>

namespace krono
{

namespace
{

portMUX_TYPE g_pulse_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_edge_mux = portMUX_INITIALIZER_UNLOCKED;
// 64-bit stores are not atomic on the P4's 32-bit cores; both sides take
// g_edge_mux.
int64_t g_edge_us = 0;
bool g_edge_pending = false;

void IRAM_ATTR on_edge(void*)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&g_edge_mux);
    g_edge_us = now;
    g_edge_pending = true;
    portEXIT_CRITICAL_ISR(&g_edge_mux);
}

bool take_edge(int64_t* out)
{
    portENTER_CRITICAL(&g_edge_mux);
    const bool pending = g_edge_pending;
    *out = g_edge_us;
    g_edge_pending = false;
    portEXIT_CRITICAL(&g_edge_mux);
    return pending;
}

}  // namespace

Clock make_gpio_pulse(int gpio, uint32_t width_us)
{
    if (gpio < 0)
    {
        return {};
    }
    // Open drain: level 1 releases the wire (Hi-Z), 0 pulls it low. Set
    // the level before the direction so the pin never drives low on setup.
    gpio_set_level(static_cast<gpio_num_t>(gpio), 1);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ull << gpio;
    io.mode = GPIO_MODE_OUTPUT_OD;
    if (gpio_config(&io) != ESP_OK)
    {
        fprintf(stderr, "synch: pulse gpio %d unusable\n", gpio);
        return {};
    }
    return [gpio = static_cast<gpio_num_t>(gpio), width_us]
    {
        // No preemption or interrupt between the stamps and the edge; the
        // edge lies between the two reads.
        portENTER_CRITICAL(&g_pulse_mux);
        const int64_t before = esp_timer_get_time();
        gpio_set_level(gpio, 0);
        const int64_t after = esp_timer_get_time();
        portEXIT_CRITICAL(&g_pulse_mux);
        esp_rom_delay_us(width_us);
        gpio_set_level(gpio, 1);
        return before + (after - before) / 2;
    };
}

EdgeSource start_gpio_edge_capture(int gpio)
{
    if (gpio < 0)
    {
        return {};
    }
    gpio_config_t io = {};
    io.pin_bit_mask = 1ull << gpio;
    io.mode = GPIO_MODE_INPUT;
    // The wire's only pull-up: the client's pin is Hi-Z between pulses, and
    // an unplugged wire reads high and raises no edges.
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.intr_type = GPIO_INTR_NEGEDGE;
    if (gpio_config(&io) != ESP_OK)
    {
        fprintf(stderr, "synch: capture gpio %d unusable\n", gpio);
        return {};
    }
    // Another component may have installed the service already.
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if ((err != ESP_OK && err != ESP_ERR_INVALID_STATE) ||
        gpio_isr_handler_add(static_cast<gpio_num_t>(gpio), on_edge, nullptr) != ESP_OK)
    {
        fprintf(stderr, "synch: capture gpio %d: no interrupt\n", gpio);
        return {};
    }
    return take_edge;
}

}  // namespace krono
