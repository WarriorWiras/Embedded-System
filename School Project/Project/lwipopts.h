#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// HTTP server settings for Pico W

// System settings
#define NO_SYS                      1
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000  // Increased for HTTP responses

// Essential protocols
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1

// Network interface settings
#define LWIP_NETIF_HOSTNAME         1

// TCP settings - important for HTTP
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)  // Increased window
#define TCP_SND_BUF                 (8 * TCP_MSS)  // Increased send buffer
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// Buffer settings - increased for HTTP
#define PBUF_POOL_SIZE              24
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_PBUF               24

// TCP timeouts
#define TCP_TMR_INTERVAL            250

// Disable unused features
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_STATS                  0

#endif /* _LWIPOPTS_H */