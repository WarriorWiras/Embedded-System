#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* --------- Hardware selection ---------
 * We use SPI0 on the Pico and wire it like this:
 *   MISO = GP16  (master in, slave out)
 *   CS   = GP17  (chip select, active low)
 *   SCK  = GP18  (clock)
 *   MOSI = GP19  (master out, slave in)
 *   BTN  = GP20  (active-low push button with pull-up)
 */
#define SPI_PORT spi0
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
#define PIN_BTN  20

/* Max sizes for the request we send and the reply we read */
#define REQ_MAX    64
#define REPLY_MAX  64

/* Simple button debounce:
 * Wait until the button is released, then sleep 50 ms.
 * (Button is wired as active-low with pull-up.)
 */
static void debounce(void) {
    while (!gpio_get(PIN_BTN)) tight_loop_contents();
    sleep_ms(50);
}

int main(void) {
    stdio_init_all();     // Enable USB/UART stdio for printf
    sleep_ms(1000);       // Give USB time to enumerate (optional)

    // --- Configure the button (GP20) as input with pull-up ---
    gpio_init(PIN_BTN);
    gpio_set_dir(PIN_BTN, GPIO_IN);
    gpio_pull_up(PIN_BTN);    // now gpio_get()==1 when not pressed, 0 when pressed

    // --- Configure CS (GP17) as output and deassert it (high) ---
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);      // CS idle high (not selected)

    // --- Configure SPI0 pins and format ---
    // Run SLOW (~20 kHz) because the slave is bit-banged in software
    spi_init(SPI_PORT, 20000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // SPI mode 0: CPOL=0, CPHA=0, 8-bit frames, MSB first
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Text we will send to the slave (terminated with '\0' when transmitted)
    const char *msg = "hello from master by wafi";

    printf("Master ready. Press GP20.\n");

    // RX buffer for reply, and a dummy TX buffer for clocking data in
    uint8_t rx[REPLY_MAX];
    uint8_t dummy[REPLY_MAX];
    memset(dummy, 0x00, sizeof dummy);   // zeros are fine for generating clocks

    while (true) {
        // Active-low button: pressed => gpio_get()==0
        if (!gpio_get(PIN_BTN)) {
            debounce();

            // ---------------- Frame 1: SEND request to slave ----------------
            // The slave expects a C string, so include the terminating '\0'.
            uint8_t req[REQ_MAX];
            size_t req_len = strlen(msg) + 1;   // include NULL terminator
            if (req_len > REQ_MAX) req_len = REQ_MAX;
            memcpy(req, msg, req_len);

            printf("Sending: %s (len=%u, includes \\0)\n", msg, (unsigned)req_len);

            // Select the slave (CS low), write bytes, then deassert CS (end of frame)
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, req, req_len);
            gpio_put(PIN_CS, 1);

            // Small delay to let the bit-banged slave switch into TX mode
            sleep_ms(3);

            // ---------------- Frame 2: READ reply from slave ----------------
            // We don't know the exact reply length, so we read up to REPLY_MAX
            // bytes by sending dummy bytes (generates SCK to shift data out of the slave).
            memset(rx, 0, sizeof rx);
            gpio_put(PIN_CS, 0);
            spi_write_read_blocking(SPI_PORT, dummy, rx, REPLY_MAX);
            gpio_put(PIN_CS, 1);

            // Dump hex view of the whole buffer (useful to debug delimiters/\0)
            printf("Raw reply: ");
            for (int i = 0; i < REPLY_MAX; i++) printf("0x%02X ", rx[i]);
            printf("\n");

            // Print ASCII up to the first '\0' (non-printables shown as '.')
            printf("Reply (ASCII): ");
            for (int i = 0; i < REPLY_MAX && rx[i] != 0; i++) {
                char c = (rx[i] >= 32 && rx[i] <= 126) ? (char)rx[i] : '.';
                putchar(c);
            }
            printf("\n");
        }
    }
}
