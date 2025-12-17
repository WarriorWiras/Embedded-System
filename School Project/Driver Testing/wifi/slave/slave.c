#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"   // CYW43 Wi-Fi chip HAL + lwIP glue
#include "lwip/udp.h"          // lwIP UDP API
#include "lwip/pbuf.h"         // lwIP packet buffers
#include "lwip/ip_addr.h"
#include "lwip/netif.h"        // to print our IP/mask/gw
#include "secrets.h"           // WIFI_SSID, WIFI_PASSWORD, MASTER_NAME, SLAVE_NAME

// ---- App settings ----
#define COMM_PORT    5555      // UDP port to listen on
#define RECV_BUF_MAX 64        // print buffer for incoming text

// Blink the Pico W Wi-Fi LED for 'ms' milliseconds
static inline void led_blink(int ms)
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    sleep_ms(ms);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

// Flags set from the UDP callback; serviced in the main loop
static volatile bool blink_rx = false;
static volatile bool blink_tx = false;
static inline void request_blink_rx(void) { blink_rx = true; }
static inline void request_blink_tx(void) { blink_tx = true; }

// Print current IPv4 info for convenience
static void print_ip_info(void)
{
    struct netif *n = netif_list;           // first (and usually only) interface
    if (!n) return;

    char ip[16], mask[16], gw[16];
    ip4addr_ntoa_r(netif_ip4_addr(n),     ip,   sizeof ip);
    ip4addr_ntoa_r(netif_ip4_netmask(n),  mask, sizeof mask);
    ip4addr_ntoa_r(netif_ip4_gw(n),       gw,   sizeof gw);
    printf("Slave: IP %s  Mask %s  GW %s\n", ip, mask, gw);
}

/* UDP receive callback (runs in the lwIP context/thread):
 * - Copy the payload into a local buffer (NUL-terminated for printf)
 * - Print sender IP:port and the message
 * - Compose a friendly reply "hi <MASTER_NAME>, this is <SLAVE_NAME>"
 * - Send reply back to the sender; set a flag to blink TX LED
 * Notes:
 *   • It's OK to call udp_sendto() from here (already in lwIP thread).
 *   • We only blink LEDs from the main loop (not from the callback).
 */
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    if (!p) return;                           // empty/EOF pbuf (ignore)
    request_blink_rx();                       // ask main loop to blink RX

    // Copy incoming payload into a printable buffer
    char buf[RECV_BUF_MAX];
    u16_t n = p->tot_len;
    if (n >= RECV_BUF_MAX) n = RECV_BUF_MAX - 1;
    pbuf_copy_partial(p, buf, n, 0);
    buf[n] = '\0';
    pbuf_free(p);                             // free lwIP pbuf(s)

    printf("Slave: from %s:%u -> '%s'\n", ipaddr_ntoa(addr), port, buf);

    // Compose reply text
    char reply[RECV_BUF_MAX];
    int rlen = snprintf(reply, sizeof(reply), "hi %s, this is %s",
                        MASTER_NAME, SLAVE_NAME);

    // Wrap reply into a pbuf and send back to the sender
    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)rlen, PBUF_RAM);
    if (!q) {
        printf("Slave: pbuf_alloc failed\n");
        return;
    }
    memcpy(q->payload, reply, rlen);

    err_t er = udp_sendto(pcb, q, addr, port); // send in-callback (safe)
    request_blink_tx();                        // ask main loop to blink TX
    pbuf_free(q);

    if (er == ERR_OK)
        printf("Slave: replied '%.*s'\n", rlen, reply);
    else
        printf("Slave: udp_sendto err %d\n", er);

    /* Note: comment below in original said "echo back exactly what we received".
       Current code sends a formatted greeting instead; that's fine—just FYI. */
}

int main()
{
    stdio_init_all();

    // Bring up CYW43 + lwIP in STA (client) mode
    if (cyw43_arch_init()) {
        printf("Slave: CYW43 init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    // Connect to Wi-Fi using secrets.h credentials (30s timeout)
    printf("Slave: connecting to WiFi '%s'...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Slave: WiFi connect failed\n");
        return 1;
    }
    printf("Slave: WiFi connected.\n");
    print_ip_info();                           // show IP so the master can target us

    // --- Create and bind a UDP PCB to COMM_PORT, install RX callback ---
    cyw43_arch_lwip_begin();                   // enter lwIP protected section
    struct udp_pcb *pcb = udp_new();
    if (!pcb) {
        cyw43_arch_lwip_end();
        printf("Slave: udp_new failed\n");
        return 1;
    }
    if (udp_bind(pcb, IP_ANY_TYPE, COMM_PORT) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        printf("Slave: udp_bind failed\n");
        return 1;
    }
    udp_recv(pcb, udp_recv_cb, NULL);          // call udp_recv_cb when packets arrive
    cyw43_arch_lwip_end();                     // leave lwIP section

    printf("Slave: listening UDP %d, waiting for data...\n", COMM_PORT);

    // Main loop: handle LED blinks (requested by callback) and keep things alive
    while (true)
    {
        if (blink_rx) { led_blink(100); blink_rx = false; } // short blink on RX
        if (blink_tx) { led_blink(150); blink_tx = false; } // slightly longer on TX
        sleep_ms(10);
    }
}
