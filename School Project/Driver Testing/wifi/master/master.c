#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"   // Wi-Fi/BT chip (CYW43) HAL + lwIP glue
#include "hardware/gpio.h"
#include "lwip/udp.h"          // lwIP UDP API
#include "lwip/pbuf.h"         // lwIP packet buffers
#include "lwip/ip_addr.h"
#include "secrets.h"           // WIFI_SSID, WIFI_PASSWORD, MASTER_NAME, SLAVE_IP
#include "lwip/netif.h"        // to print our IP

// ---- Hardware / protocol settings ----
#define BTN_PIN 20            // Maker Pi Pico W user button (active-low)
#define COMM_PORT 5555        // UDP port used by both ends
#define RECV_BUF_MAX 64       // local buffer to print a reply
#define REPLY_TIMEOUT_MS 5000 // wait up to 5s for slave’s reply
#define MAX_MSG 64            // max size of the message we send

// Small struct to keep app state for the UDP callback
typedef struct
{
    struct udp_pcb *pcb;         // lwIP UDP control block (socket-ish handle)
    volatile bool got_reply;     // set true when a reply arrives
    char recv_buf[RECV_BUF_MAX]; // stores the last received text (for printf)
} app_state_t;

/* Wait for a clean, debounced button press on GP20.
 * - Button is wired as active-low: pressed => gpio_get()==0.
 * - We require the level to be stable low for ~30ms, then wait for release. */
static void wait_for_button_press(void)
{
    gpio_init(BTN_PIN);
    gpio_set_dir(BTN_PIN, GPIO_IN);
    gpio_pull_up(BTN_PIN);

    bool last = gpio_get(BTN_PIN);
    absolute_time_t stable = get_absolute_time();

    while (true)
    {
        bool now = gpio_get(BTN_PIN);
        if (now != last) {                // edge detected: reset stability timer
            last = now;
            stable = get_absolute_time();
        }
        // Pressed and stable for >30ms? break.
        if (!now && absolute_time_diff_us(stable, get_absolute_time()) > 30000)
            break;
        sleep_ms(5);
    }

    // Wait for the user to release the button before returning
    while (!gpio_get(BTN_PIN))
        sleep_ms(5);
}

/* UDP receive callback:
 * - Copies payload (up to RECV_BUF_MAX-1), NUL-terminates it,
 * - sets got_reply=true so main loop stops waiting,
 * - prints sender IP:port and the text. */
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    app_state_t *st = (app_state_t *)arg;
    if (!p) return;                         // empty pbuf (ignore)

    u16_t n = p->tot_len;                   // total bytes in chain
    if (n >= RECV_BUF_MAX) n = RECV_BUF_MAX - 1;
    pbuf_copy_partial(p, st->recv_buf, n, 0);
    st->recv_buf[n] = '\0';
    pbuf_free(p);                           // free the packet buffer(s)

    st->got_reply = true;
    printf("Master: reply from %s:%u -> '%s'\n",
           ipaddr_ntoa(addr), port, st->recv_buf);
}

int main()
{
    stdio_init_all();

    // --- Bring up Wi-Fi stack (CYW43 + lwIP) in STA mode ---
    if (cyw43_arch_init()) {
        printf("Master: CYW43 init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    // --- Connect to your Wi-Fi (secrets.h) ---
    printf("Master: connecting to WiFi '%s'...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Master: WiFi connect failed\n");
        return 1;
    }
    printf("Master: WiFi connected.\n");

    // Print our IP address so you can verify network config
    struct netif *n = netif_list;
    if (n) printf("Master: IP %s\n", ipaddr_ntoa(netif_ip_addr4(n)));

    // --- Resolve destination IP from secrets.h (string -> ip_addr_t) ---
    app_state_t st = (app_state_t){0};
    ip_addr_t dest_ip;
    if (!ipaddr_aton(SLAVE_IP, &dest_ip)) {
        printf("Master: bad SLAVE_IP\n");
        return 1;
    }

    // --- Create a UDP PCB, bind it (ephemeral port), and install RX callback ---
    cyw43_arch_lwip_begin();                // enter lwIP critical section
    st.pcb = udp_new();
    if (!st.pcb) {
        cyw43_arch_lwip_end();
        printf("Master: udp_new failed\n");
        return 1;
    }
    if (udp_bind(st.pcb, IP_ANY_TYPE, 0) != ERR_OK) { // 0 = choose a local port
        udp_remove(st.pcb);
        cyw43_arch_lwip_end();
        printf("Master: udp_bind failed\n");
        return 1;
    }
    udp_recv(st.pcb, udp_recv_cb, &st);     // callback will run on incoming packets
    cyw43_arch_lwip_end();                  // leave lwIP section

    // Blink the onboard Wi-Fi LED once (power-on indicator)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(100);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    printf("Master: Press GP20 to send 'hello from %s' to %s:%d\n",
           MASTER_NAME, SLAVE_IP, COMM_PORT);

    // --- Main loop: wait for button -> send UDP -> wait for reply or timeout ---
    while (true)
    {
        wait_for_button_press();

        // Compose the message "hello from <MASTER_NAME>"
        char msg[MAX_MSG];
        int msg_len = snprintf(msg, MAX_MSG, "hello from %s", MASTER_NAME);

        // Allocate a pbuf for the UDP payload and copy the message into it
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)msg_len, PBUF_RAM);
        if (!p) {
            printf("Master: pbuf_alloc failed\n");
            continue;
        }
        memcpy(p->payload, msg, msg_len);

        // Reset reply flag; send the packet to SLAVE_IP:COMM_PORT
        st.got_reply = false;
        cyw43_arch_lwip_begin();
        err_t er = udp_sendto(st.pcb, p, &dest_ip, COMM_PORT);
        cyw43_arch_lwip_end();

        printf("Master: src %u -> %s:%d\n", st.pcb->local_port, SLAVE_IP, COMM_PORT);
        pbuf_free(p); // free our payload buffer (lwIP copies/queues internally)

        if (er != ERR_OK) {
            printf("Master: udp_sendto err %d\n", er);
            continue;
        }
        printf("Master: sent '%.*s', waiting...\n", msg_len, msg);

        // Wait for the RX callback to set got_reply=true, or time out after 5s
        absolute_time_t deadline = make_timeout_time_ms(REPLY_TIMEOUT_MS);
        while (!st.got_reply && absolute_time_diff_us(get_absolute_time(), deadline) > 0)
            sleep_ms(10);

        if (!st.got_reply)
            printf("Master: timeout waiting for reply\n");

        printf("Master: ready. Press GP20 again.\n");
    }
}
