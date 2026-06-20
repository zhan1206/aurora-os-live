#include "include/log.h"
#include "include/portio.h"
#include <stdint.h>

#define PIT_BASE_FREQ   1193180
#define PIT_MIN_FREQ    18      /* ~18.2 Hz minimum (divisor=65535) */
#define PIT_MAX_FREQ    1193180 /* 1.193 MHz maximum (divisor=1) */

int pit_init(uint32_t freq_hz) {
    if (freq_hz == 0) freq_hz = 100;  /* default to 100 Hz */
    if (freq_hz < PIT_MIN_FREQ) {
        log_printf(LOG_LEVEL_WARN, "pit: freq %u Hz too low, clamped to %u Hz\n",
                   freq_hz, PIT_MIN_FREQ);
        freq_hz = PIT_MIN_FREQ;
    }
    if (freq_hz > PIT_MAX_FREQ) {
        log_printf(LOG_LEVEL_WARN, "pit: freq %u Hz too high, clamped to %u Hz\n",
                   freq_hz, PIT_MAX_FREQ);
        freq_hz = PIT_MAX_FREQ;
    }

    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;

    outb(0x43, 0x36); /* channel 0, lobyte/hibyte, mode 3, binary */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    log_printf(LOG_LEVEL_DEBUG, "pit: initialized at %u Hz (divisor=%u)\n", freq_hz, divisor);
    return 0;
}
