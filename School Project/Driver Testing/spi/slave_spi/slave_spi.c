#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* ----------------------------------------------------------------------
 * Software (bit-banged) SPI SLAVE — SPI mode 0 (CPOL=0, CPHA=0)
 * Wiring matches a Pico master using SPI0:
 *   PIN_MISO = GP16  (slave out -> master MISO)
 *   PIN_CS   = GP17  (chip select from master, active LOW)
 *   PIN_SCK  = GP18  (clock from master)
 *   PIN_MOSI = GP19  (data from master)
 *
 * Protocol used by the paired master:
 *   Frame 1 (CS low): master sends a C-string (NUL-terminated).
 *   Frame 2 (CS low): master clocks out the slave's prepared reply (also NUL-terminated).
 *
 * Notes:
 * - Mode 0 timing: slave puts data on MISO while SCK is LOW, master samples on RISING edges.
 * - Keep master SCK slow (e.g., ~20 kHz) so these GPIO loops can keep up.
 * ---------------------------------------------------------------------- */

#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19

#define MAX_MSG_SIZE 100

/* What we’ll send back to the master on the second frame. */
static const char *reply_str = "hi master, this is slave by wafi";

/* Shorthand helpers to read CS state (active-low). */
static inline bool cs_is_low(void)  { return gpio_get(PIN_CS) == 0; }
static inline bool cs_is_high(void) { return gpio_get(PIN_CS) != 0; }

/* Wait until SCK rises (0->1). Abort if the master deasserts CS mid-wait. */
static inline bool wait_sck_rise_abort_on_cs(void) {
    while (gpio_get(PIN_SCK) == 0) { if (cs_is_high()) return false; }
    return true;
}

/* Wait until SCK falls (1->0). Abort if the master deasserts CS mid-wait. */
static inline bool wait_sck_fall_abort_on_cs(void) {
    while (gpio_get(PIN_SCK) != 0) { if (cs_is_high()) return false; }
    return true;
}

/* ----------------------------------------------------------------------
 * Receive one byte from the master (MODE 0):
 * - Sample MOSI on each RISING edge (master changes data while SCK low).
 * - Return false if CS goes high in the middle of a byte (abort).
 * ---------------------------------------------------------------------- */
static bool spi_slave_recv_byte(uint8_t *out) {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        if (!wait_sck_rise_abort_on_cs()) return false;              // sample on rising edge
        b = (uint8_t)((b << 1) | (gpio_get(PIN_MOSI) & 1));          // shift in MOSI bit
        if (!wait_sck_fall_abort_on_cs()) return false;              // wait for falling edge
    }
    *out = b;
    return true;
}

/* ----------------------------------------------------------------------
 * Transmit one byte to the master (MODE 0):
 * - Present the next bit while SCK is LOW.
 * - Master samples on the following RISING edge.
 * - Return false if CS goes high mid-byte.
 * ---------------------------------------------------------------------- */
static bool spi_slave_send_byte(uint8_t b) {
    for (int bit = 7; bit >= 0; bit--) {
        // Wait for SCK to be LOW before putting the next bit on MISO
        while (gpio_get(PIN_SCK) != 0) { if (cs_is_high()) return false; }
        gpio_put(PIN_MISO, (b >> bit) & 1);                          // drive next bit
        if (!wait_sck_rise_abort_on_cs()) return false;              // master samples here
        if (!wait_sck_fall_abort_on_cs()) return false;              // complete the clock
    }
    return true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(1000);  // allow USB serial to enumerate

    // Inputs from master
    gpio_init(PIN_SCK);  gpio_set_dir(PIN_SCK,  GPIO_IN);
    gpio_init(PIN_MOSI); gpio_set_dir(PIN_MOSI, GPIO_IN);
    gpio_init(PIN_CS);   gpio_set_dir(PIN_CS,   GPIO_IN);
    gpio_pull_up(PIN_CS);          // idle CS high (not selected)

    // Output to master
    gpio_init(PIN_MISO); gpio_set_dir(PIN_MISO, GPIO_OUT);
    gpio_put(PIN_MISO, 0);         // idle MISO low (mode 0 convention)

    printf("Soft SPI slave ready (mode 0)\n");

    uint8_t request[MAX_MSG_SIZE]; // buffer for what master sends (frame 1)
    uint8_t reply[MAX_MSG_SIZE];   // buffer we transmit back (frame 2)
    size_t  rep_len = 0;           // reply length (incl. NUL)
    bool    have_reply = false;    // toggles between RX phase and TX phase

    while (true) {
        /* -------- Wait for CS falling edge = start of a frame -------- */
        while (cs_is_high()) tight_loop_contents();
        sleep_us(2); // small settle time after CS goes low

        if (!have_reply) {
            /* ======================= RX FRAME ======================== */
            size_t req_len = 0;
            while (cs_is_low()) {
                uint8_t b = 0;
                if (!spi_slave_recv_byte(&b)) break;                 // abort if CS rises mid-byte
                if (req_len < MAX_MSG_SIZE - 1) request[req_len++] = b;
                if (b == 0) break;                                   // stop on NUL terminator
            }
            request[req_len] = 0;                                    // ensure C-string

            // Log the received text (non-printables shown as '.')
            printf("Slave got %u bytes: ", (unsigned)req_len);
            for (size_t i = 0; i < req_len; i++) {
                char c = (request[i] >= 32 && request[i] <= 126) ? request[i] : '.';
                putchar(c);
            }
            printf("\n");

            // Prepare the reply for the next frame (include trailing NUL)
            rep_len = strlen(reply_str) + 1;
            if (rep_len > MAX_MSG_SIZE) rep_len = MAX_MSG_SIZE;
            memcpy(reply, reply_str, rep_len);
            have_reply = true;

            // Consume the rest of this frame (wait until CS returns high)
            while (cs_is_low()) tight_loop_contents();
            printf("Reply prepared (%u bytes incl. \\0)\n", (unsigned)rep_len);

        } else {
            /* ======================= TX FRAME ======================== */
            size_t idx = 0;
            while (cs_is_low() && idx < rep_len) {
                if (!spi_slave_send_byte(reply[idx])) break;         // abort if CS rises mid-byte
                idx++;
            }
            gpio_put(PIN_MISO, 0);                                   // idle low after frame
            have_reply = false;                                      // next frame will be RX again

            // Wait for CS to return high (frame end), then log
            while (cs_is_low()) tight_loop_contents();
            printf("Reply sent (%u bytes)\n", (unsigned)idx);
        }
    }
}
