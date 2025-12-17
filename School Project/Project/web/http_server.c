#include "http_server.h"
#include "config/config.h"
#include "sd_card.h"
#include "fatfs/ff.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "pico/cyw43_arch.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// HTTP server state structure
typedef struct {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    bool sending_file;
    FIL file;
    uint32_t bytes_sent;
    uint32_t total_size;
    bool headers_sent;
} http_server_t;

// Global state
static struct tcp_pcb *http_server = NULL;
static http_server_t *current_file_state = NULL;

// External references (from main.c)
static sd_file_info_t *http_file_list = NULL;
static int *http_file_count_ptr = NULL;
static bool *http_file_list_needs_refresh_ptr = NULL;

// External function declarations (implemented in main.c)
extern float http_get_temperature(void);
extern float http_get_voltage(void);
extern bool http_get_sd_mounted(void);
extern int http_get_file_list(sd_file_info_t *files, int max_files);

// Simple HTTP response builder
static void send_http_response(struct tcp_pcb *pcb, const char *content_type, 
                               const char *content, size_t content_len, 
                               const char *filename) {
    char headers[512];
    int header_len;
    
    if (filename) {
        header_len = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type, filename, content_len);
    } else {
        header_len = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type, content_len);
    }
    
    tcp_write(pcb, headers, header_len, TCP_WRITE_FLAG_COPY);
    
    size_t sent = 0;
    while (sent < content_len) {
        size_t chunk_size = (content_len - sent) > 1024 ? 1024 : (content_len - sent);
        tcp_write(pcb, content + sent, chunk_size, TCP_WRITE_FLAG_COPY);
        sent += chunk_size;
    }
    
    tcp_output(pcb);
}

// Progress tracking for file transfers
static uint32_t last_reported_percent = 0;
static uint32_t last_reported_bytes = 0;

// Send file chunk over HTTP
static err_t http_send_file_chunk(struct tcp_pcb *pcb, http_server_t *state) {
    if (!state || !state->sending_file) {
        return ERR_OK;
    }
    
    if (!state->headers_sent) {
        state->headers_sent = true;
    }
    
    u16_t available = tcp_sndbuf(pcb);
    if (available < 512) {
        return ERR_OK;
    }
    
    static uint8_t buffer[2048];
    UINT to_read = available;
    if (to_read > sizeof(buffer)) to_read = sizeof(buffer);
    
    uint32_t remaining = state->total_size - state->bytes_sent;
    if (to_read > remaining) to_read = remaining;
    
    if (to_read == 0) {
        f_close(&state->file);
        state->sending_file = false;
        printf("\n[+] File transfer complete: 100%% (%lu / %lu bytes)\n", 
               (unsigned long)state->bytes_sent, (unsigned long)state->total_size);
        last_reported_percent = 0;
        last_reported_bytes = 0;
        return ERR_OK;
    }
    
    UINT bytes_read = 0;
    FRESULT fr = f_read(&state->file, buffer, to_read, &bytes_read);
    
    if (fr != FR_OK || bytes_read == 0) {
        f_close(&state->file);
        state->sending_file = false;
        if (fr == FR_OK) {
            printf("\n[+] File transfer complete: 100%% (%lu / %lu bytes)\n", 
                   (unsigned long)state->bytes_sent, (unsigned long)state->total_size);
        } else {
            printf("\n[!] File read error: %d\n", fr);
        }
        last_reported_percent = 0;
        last_reported_bytes = 0;
        return ERR_OK;
    }
    
    uint32_t bytes_before = state->bytes_sent;
    state->bytes_sent += bytes_read;
    
    static uint32_t last_reported_percent_static = 0;
    static uint32_t last_reported_bytes_static = 0;
    uint32_t current_percent = (state->total_size > 0) ? 
        (state->bytes_sent * 100) / state->total_size : 0;
    uint32_t bytes_since_report = state->bytes_sent - last_reported_bytes_static;
    
    if (state->bytes_sent < last_reported_bytes_static) {
        last_reported_percent_static = 0;
        last_reported_bytes_static = 0;
    }
    
    if (current_percent >= last_reported_percent_static + 5 || bytes_since_report >= 51200) {
        char size_str[32];
        char sent_str[32];
        if (state->total_size < 1024) {
            snprintf(size_str, sizeof(size_str), "%lu B", (unsigned long)state->total_size);
            snprintf(sent_str, sizeof(sent_str), "%lu B", (unsigned long)state->bytes_sent);
        } else if (state->total_size < 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%.2f KB", state->total_size / 1024.0f);
            snprintf(sent_str, sizeof(sent_str), "%.2f KB", state->bytes_sent / 1024.0f);
        } else {
            snprintf(size_str, sizeof(size_str), "%.2f MB", state->total_size / (1024.0f * 1024.0f));
            snprintf(sent_str, sizeof(sent_str), "%.2f MB", state->bytes_sent / (1024.0f * 1024.0f));
        }
        
        printf("[*] Download progress: %3lu%% (%s / %s)        \r", 
               (unsigned long)current_percent, sent_str, size_str);
        fflush(stdout);
        last_reported_percent_static = current_percent;
        last_reported_bytes_static = state->bytes_sent;
    }
    
    err_t err = tcp_write(pcb, buffer, bytes_read, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        tcp_output(pcb);
    } else if (err == ERR_MEM) {
        state->bytes_sent = bytes_before;
        uint32_t current_pos = state->file.fptr;
        f_lseek(&state->file, current_pos - bytes_read);
    }
    
    return err;
}

// TCP sent callback
static err_t http_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    http_server_t *state = (http_server_t *)arg;
    if (state && state->sending_file) {
        http_send_file_chunk(tpcb, state);
    }
    return ERR_OK;
}

// HTTP error callback
static void http_server_err(void *arg, err_t err) {
    http_server_t *state = (http_server_t *)arg;
    if (state) {
        if (state->sending_file) {
            f_close(&state->file);
            state->sending_file = false;
        }
        state->client_pcb = NULL;
        if (current_file_state == state) {
            current_file_state = NULL;
        }
        free(state);
        printf("[*] HTTP client disconnected (error: %d)\n", err);
    }
}

// HTTP receive callback
static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        if (current_file_state) {
            if (current_file_state->sending_file) {
                f_close(&current_file_state->file);
            }
            free(current_file_state);
            current_file_state = NULL;
        }
        tcp_close(pcb);
        return ERR_OK;
    }
    
    char request[256] = {0};
    size_t copy_len = (p->tot_len < 255) ? p->tot_len : 255;
    pbuf_copy_partial(p, request, copy_len, 0);
    
    printf("[*] HTTP Request: ");
    char *end = strchr(request, '\r');
    if (end) *end = '\0';
    printf("%s\n", request);
    
    // Check for file download requests
    char *file_param = strstr(request, "GET /file?name=");
    if (file_param) {
        char filename[64] = {0};
        char *name_start = file_param + strlen("GET /file?name=");
        char *name_end = strchr(name_start, ' ');
        if (!name_end) name_end = strchr(name_start, '\r');
        if (!name_end) name_end = strchr(name_start, '\n');
        if (!name_end) name_end = strchr(name_start, '&');
        
        if (name_end) {
            size_t name_len = name_end - name_start;
            if (name_len < sizeof(filename)) {
                memcpy(filename, name_start, name_len);
                filename[name_len] = '\0';
                
                // URL decode
                char decoded[64] = {0};
                int j = 0;
                for (int i = 0; filename[i] && j < sizeof(decoded) - 1; i++) {
                    if (filename[i] == '%' && filename[i+1] && filename[i+2]) {
                        char hex[3] = {filename[i+1], filename[i+2], 0};
                        decoded[j++] = (char)strtol(hex, NULL, 16);
                        i += 2;
                    } else if (filename[i] == '+') {
                        decoded[j++] = ' ';
                    } else {
                        decoded[j++] = filename[i];
                    }
                }
                
                // Open file for streaming
                if (http_get_sd_mounted() && sd_file_exists(decoded)) {
                    http_server_t *state = (http_server_t *)malloc(sizeof(http_server_t));
                    if (!state) {
                        const char *error = "HTTP/1.1 500 Internal Server Error\r\n\r\nOut of memory\r\n";
                        tcp_write(pcb, error, strlen(error), TCP_WRITE_FLAG_COPY);
                        tcp_output(pcb);
                        tcp_recved(pcb, p->tot_len);
                        pbuf_free(p);
                        return ERR_OK;
                    }
                    
                    memset(state, 0, sizeof(http_server_t));
                    state->client_pcb = pcb;
                    state->sending_file = false;
                    state->headers_sent = false;
                    state->bytes_sent = 0;
                    
                    FRESULT fr = f_open(&state->file, decoded, FA_OPEN_EXISTING | FA_READ);
                    if (fr == FR_OK) {
                        f_lseek(&state->file, 0);
                        state->total_size = f_size(&state->file);
                        state->sending_file = true;
                        current_file_state = state;
                        
                        printf("[*] File opened: %s, size=%lu bytes\n", 
                               decoded, (unsigned long)state->total_size);
                        
                        const char *content_type = "application/octet-stream";
                        if (strstr(decoded, ".csv") || strstr(decoded, ".CSV")) {
                            content_type = "text/csv";
                        } else if (strstr(decoded, ".txt") || strstr(decoded, ".TXT")) {
                            content_type = "text/plain";
                        }
                        
                        char headers[512];
                        int header_len = snprintf(headers, sizeof(headers),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Disposition: attachment; filename=\"%s\"\r\n"
                            "Content-Length: %lu\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            content_type, decoded, (unsigned long)state->total_size);
                        
                        tcp_write(pcb, headers, header_len, TCP_WRITE_FLAG_COPY);
                        tcp_output(pcb);
                        state->headers_sent = true;
                        
                        tcp_sent(pcb, http_server_sent);
                        tcp_err(pcb, http_server_err);
                        tcp_arg(pcb, state);
                        
                        http_send_file_chunk(pcb, state);
                        
                        printf("\n[+] Started file transfer: %s\n", decoded);
                        printf("[*] Download progress: 0%% (0 / %lu bytes)\r", (unsigned long)state->total_size);
                    } else {
                        free(state);
                        const char *error = "HTTP/1.1 500 Internal Server Error\r\n\r\nFailed to open file\r\n";
                        tcp_write(pcb, error, strlen(error), TCP_WRITE_FLAG_COPY);
                        tcp_output(pcb);
                    }
                } else {
                    const char *error = "HTTP/1.1 404 Not Found\r\n\r\nFile not found\r\n";
                    tcp_write(pcb, error, strlen(error), TCP_WRITE_FLAG_COPY);
                    tcp_output(pcb);
                }
            } else {
                const char *error = "HTTP/1.1 400 Bad Request\r\n\r\nFilename too long\r\n";
                tcp_write(pcb, error, strlen(error), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
            }
        } else {
            const char *error = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid request format\r\n";
            tcp_write(pcb, error, strlen(error), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
        }
    } else {
        // Send HTML page with file list
        if (http_get_sd_mounted()) {
            if (http_file_list && http_file_count_ptr) {
                int old_file_count = *http_file_count_ptr;
                *http_file_count_ptr = http_get_file_list(http_file_list, MAX_FILES_TO_LIST);
                
                if (old_file_count != *http_file_count_ptr) {
                    printf("[*] File list updated: %d files (was %d)\n", *http_file_count_ptr, old_file_count);
                } else {
                    printf("[*] File list refreshed: %d files\n", *http_file_count_ptr);
                }
                if (http_file_list_needs_refresh_ptr) {
                    *http_file_list_needs_refresh_ptr = false;
                }
            }
        }
        
        // Build HTML
        char html[4096];
        int html_pos = snprintf(html, sizeof(html),
            "<!DOCTYPE html><html><head>"
            "<title>IS16 Website</title>"
            "<meta http-equiv='refresh' content='5'>"
            "<style>"
            "body{font-family:Arial;margin:20px;background:#B0E0E6;color:#000000;}"  // Pastel blue background, black text
            "h1{color:#000000;}"  // Black heading
            "h2{color:#000000;}"  // Black subheading
            "p{color:#000000;}"   // Black paragraph text
            ".box{background:#E0F7FA;padding:20px;border-radius:10px;margin:20px 0;box-shadow:0 2px 4px rgba(0,0,0,0.1);border:1px solid #B0D4E1;}"  // Lighter pastel blue boxes
            "button{background:#4CAF50;color:white;padding:10px 20px;border:none;"
            "border-radius:5px;font-size:14px;cursor:pointer;margin:5px;}"
            "button:hover{background:#45a049;}"
            "table{width:100%%;border-collapse:collapse;background:#E0F7FA;}"  // Pastel blue table background
            "th,td{padding:10px;text-align:left;border-bottom:1px solid #B0D4E1;color:#000000;}"  // Black text in table
            "th{background:#81C7D4;color:#000000;font-weight:bold;}"  // Slightly darker pastel blue for headers, black text
            "tr:hover{background:#B0E0E6;}"  // Hover effect with pastel blue
            ".status{color:#006400;font-weight:bold;}"  // Dark green for status (readable on pastel blue)
            ".status.error{color:#8B0000;}"  // Dark red for errors (readable on pastel blue)
            "a{color:#0066CC;}"  // Blue links for visibility
            "a:visited{color:#0066CC;}"
            "</style></head><body>"
            "<h1>[INF2004] Project: SPI Flash Performance Evaluation & Forensic Analysis</h1>"
            "<div class='box'>"
            "<h2>System Status</h2>"
            "<p><strong>SD Card:</strong> <span class='status %s'>%s</span></p>"
            "<p><strong>Temperature:</strong> %.1f°C</p>"
            "<p><strong>Voltage:</strong> %.2fV</p>"
            "<p><strong>Files Found:</strong> %d</p>"
            "</div>"
            "<div class='box'>"
            "<h2>Files on SD Card</h2>",
            http_get_sd_mounted() ? "" : "error",
            http_get_sd_mounted() ? "MOUNTED" : "NOT MOUNTED",
            http_get_temperature(),
            http_get_voltage(),
            http_file_count_ptr ? *http_file_count_ptr : 0);
        
        if (!http_get_sd_mounted()) {
            html_pos += snprintf(html + html_pos, sizeof(html) - html_pos,
                "<p style='color:#8B0000;'> SD card not mounted. Press GP20 to scan.</p>");
        } else if (!http_file_count_ptr || *http_file_count_ptr == 0) {
            html_pos += snprintf(html + html_pos, sizeof(html) - html_pos,
                "<p style='color:#000000;'>No files found on SD card.</p>");
        } else {
            html_pos += snprintf(html + html_pos, sizeof(html) - html_pos,
                "<table><tr><th>Filename</th><th>Size</th><th>Action</th></tr>");
            
            for (int i = 0; i < *http_file_count_ptr && html_pos < sizeof(html) - 200 && http_file_list; i++) {
                char size_str[32];
                if (http_file_list[i].size < 1024) {
                    snprintf(size_str, sizeof(size_str), "%lu B", (unsigned long)http_file_list[i].size);
                } else if (http_file_list[i].size < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.2f KB", http_file_list[i].size / 1024.0f);
                } else {
                    snprintf(size_str, sizeof(size_str), "%.2f MB", http_file_list[i].size / (1024.0f * 1024.0f));
                }
                
                char encoded[64] = {0};
                int enc_pos = 0;
                for (int j = 0; http_file_list[i].filename[j] && enc_pos < sizeof(encoded) - 3; j++) {
                    char c = http_file_list[i].filename[j];
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
                        encoded[enc_pos++] = c;
                    } else if (c == ' ') {
                        encoded[enc_pos++] = '+';
                    } else {
                        enc_pos += snprintf(encoded + enc_pos, sizeof(encoded) - enc_pos, "%%%02X", (unsigned char)c);
                    }
                }
                
                html_pos += snprintf(html + html_pos, sizeof(html) - html_pos,
                    "<tr><td>%s</td><td>%s</td>"
                    "<td><a href='/file?name=%s'><button> Download</button></a></td></tr>",
                    http_file_list[i].filename, size_str, encoded);
            }
            
            html_pos += snprintf(html + html_pos, sizeof(html) - html_pos, "</table>");
        }
        
        html_pos += snprintf(html + html_pos, sizeof(html) - html_pos,
            "</div>"
            "<div class='box' style='background:#E0F7FA;'>"
            "<p style='color:#000000;'>Connected to: %s<br>"
            "IP: 192.168.4.1<br>"
            "Press GP20 on device to refresh file list<br>"
            "Page auto-refreshes every 5 seconds</p>"
            "</div>"
            "</body></html>",
            AP_SSID);
        
        send_http_response(pcb, "text/html", html, html_pos, NULL);
        printf("[+] Sent HTML page with %d files\n", http_file_count_ptr ? *http_file_count_ptr : 0);
    }
    
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    
    if (!current_file_state || !current_file_state->sending_file) {
        tcp_close(pcb);
    }
    
    return ERR_OK;
}

// HTTP error callback (for accept)
static void http_err(void *arg, err_t err) {
    printf("[!] HTTP connection error: %d\n", err);
}

// HTTP accept callback
static err_t http_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    if (err != ERR_OK || client_pcb == NULL) {
        return ERR_VAL;
    }
    
    printf("[+] HTTP client connected\n");
    
    tcp_recv(client_pcb, http_recv);
    tcp_err(client_pcb, http_err);
    
    return ERR_OK;
}

// Initialize HTTP server
bool http_server_init(void) {
    http_server = tcp_new();
    if (!http_server) {
        printf("[!] Failed to create HTTP server\n");
        return false;
    }
    
    err_t err = tcp_bind(http_server, IP_ADDR_ANY, HTTP_PORT);
    if (err != ERR_OK) {
        printf("[!] Failed to bind HTTP port: %d\n", err);
        return false;
    }
    
    http_server = tcp_listen(http_server);
    if (!http_server) {
        printf("[!] Failed to listen\n");
        return false;
    }
    
    tcp_accept(http_server, http_accept);
    
    printf("[+] HTTP server running at http://192.168.4.1\n");
    return true;
}

// Set file list pointers (called from main.c)
void http_server_set_file_list(sd_file_info_t *files, int *file_count, bool *needs_refresh) {
    http_file_list = files;
    http_file_count_ptr = file_count;
    http_file_list_needs_refresh_ptr = needs_refresh;
}

