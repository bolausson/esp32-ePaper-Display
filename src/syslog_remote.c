/**
 * @file syslog_remote.c
 * @brief Remote syslog forwarding for ESP-IDF log output
 *
 * Uses a FreeRTOS queue + dedicated sender task to decouple the
 * vprintf hook from the network stack. This prevents contention
 * between syslog UDP/TCP sends and ongoing TLS operations.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "syslog_remote.h"

static const char *TAG = "SYSLOG";

// Syslog facility: user-level (1)
#define SYSLOG_FACILITY 1
#define SYSLOG_QUEUE_SIZE 32
#define SYSLOG_TASK_STACK 4096
#define SYSLOG_MSG_MAX 512

// Queued message
typedef struct {
    char text[SYSLOG_MSG_MAX];
    int len;
} syslog_msg_t;

// Module state
static bool s_active = false;
static int s_sock = -1;
static struct sockaddr_in s_server_addr;
static syslog_format_t s_format = SYSLOG_FMT_RFC3164;
static syslog_transport_t s_transport = SYSLOG_TRANSPORT_UDP;
static char s_hostname[64] = {0};
static vprintf_like_t s_original_vprintf = NULL;
static bool s_in_hook = false;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;

// Map ESP log level character to RFC 5424 severity
static int esp_level_to_severity(char level_char) {
    switch (level_char) {
        case 'E': return 3;  // Error
        case 'W': return 4;  // Warning
        case 'I': return 6;  // Informational
        case 'D': return 7;  // Debug
        case 'V': return 7;  // Debug (verbose)
        default:  return 6;  // Default to informational
    }
}

// Format RFC 3164 syslog message: <PRI>Mmm dd HH:MM:SS HOSTNAME APP: MSG
static int format_rfc3164(char *out, size_t out_size, int priority, const char *msg) {
    struct timeval tv;
    struct tm timeinfo;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &timeinfo);

    if (timeinfo.tm_year + 1900 < 2025) {
        return snprintf(out, out_size, "<%d>Jan  1 00:00:00 %s esp32-display: %s",
                        priority, s_hostname, msg);
    }

    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%b %e %H:%M:%S", &timeinfo);

    return snprintf(out, out_size, "<%d>%s %s esp32-display: %s",
                    priority, timestamp, s_hostname, msg);
}

// Format RFC 5424 syslog message: <PRI>1 TIMESTAMP HOSTNAME APP - - - MSG
static int format_rfc5424(char *out, size_t out_size, int priority, const char *msg) {
    struct timeval tv;
    struct tm timeinfo;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &timeinfo);

    if (timeinfo.tm_year + 1900 < 2025) {
        return snprintf(out, out_size, "<%d>1 - %s esp32-display - - - %s",
                        priority, s_hostname, msg);
    }

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &timeinfo);

    int ms = tv.tv_usec / 1000;
    char tz_offset[8];

    struct tm utcinfo;
    gmtime_r(&tv.tv_sec, &utcinfo);
    int local_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int utc_min = utcinfo.tm_hour * 60 + utcinfo.tm_min;
    int diff_min = local_min - utc_min;
    if (diff_min > 720) diff_min -= 1440;
    if (diff_min < -720) diff_min += 1440;

    if (diff_min == 0) {
        strcpy(tz_offset, "Z");
    } else {
        int abs_min = abs(diff_min);
        snprintf(tz_offset, sizeof(tz_offset), "%c%02d:%02d",
                 diff_min > 0 ? '+' : '-', abs_min / 60, abs_min % 60);
    }

    return snprintf(out, out_size, "<%d>1 %s.%03d%s %s esp32-display - - - %s",
                    priority, timestamp, ms, tz_offset, s_hostname, msg);
}

// Send a formatted syslog packet over the network (runs in sender task only)
static void syslog_net_send(const char *data, int len) {
    if (s_sock == -1) return;

    if (s_transport == SYSLOG_TRANSPORT_UDP) {
        sendto(s_sock, data, len, MSG_DONTWAIT,
               (struct sockaddr *)&s_server_addr, sizeof(s_server_addr));
    } else {
        // TCP with RFC 6587 octet-counting: "LEN MSG"
        char frame_header[16];
        int hdr_len = snprintf(frame_header, sizeof(frame_header), "%d ", len);

        int ret = send(s_sock, frame_header, hdr_len, MSG_DONTWAIT);
        if (ret < 0) {
            close(s_sock);
            s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s_sock >= 0) {
                struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                if (connect(s_sock, (struct sockaddr *)&s_server_addr, sizeof(s_server_addr)) < 0) {
                    close(s_sock);
                    s_sock = -1;
                    return;
                }
                ret = send(s_sock, frame_header, hdr_len, MSG_DONTWAIT);
                if (ret < 0) return;
            } else {
                s_sock = -1;
                return;
            }
        }
        send(s_sock, data, len, MSG_DONTWAIT);
    }
}

// Sender task: reads from queue and sends over network
static void syslog_sender_task(void *arg) {
    syslog_msg_t msg;
    char syslog_buf[768];

    while (s_active) {
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Parse level from formatted ESP_LOG output
            const char *p = msg.text;
            while (*p == '\033') {
                while (*p && *p != 'm') p++;
                if (*p == 'm') p++;
            }

            int severity = esp_level_to_severity(*p);
            int priority = SYSLOG_FACILITY * 8 + severity;

            // Strip trailing newline/whitespace and ANSI reset codes
            char *end = msg.text + strlen(msg.text);
            while (end > msg.text) {
                end--;
                if (*end == '\n' || *end == '\r' || *end == ' ') {
                    *end = '\0';
                } else if (*end == 'm' && end >= msg.text + 3 && *(end - 3) == '\033') {
                    *(end - 3) = '\0';
                    end = msg.text + strlen(msg.text);
                } else {
                    break;
                }
            }

            // Format and send
            int syslog_len;
            if (s_format == SYSLOG_FMT_RFC5424) {
                syslog_len = format_rfc5424(syslog_buf, sizeof(syslog_buf), priority, msg.text);
            } else {
                syslog_len = format_rfc3164(syslog_buf, sizeof(syslog_buf), priority, msg.text);
            }

            if (syslog_len > 0 && syslog_len < (int)sizeof(syslog_buf)) {
                syslog_net_send(syslog_buf, syslog_len);
            }
        }
    }

    vTaskDelete(NULL);
}

// vprintf hook — intercepts all ESP_LOG* output, enqueues for async send
static int syslog_vprintf_hook(const char *fmt, va_list args) {
    if (s_active && !s_in_hook && s_queue) {
        s_in_hook = true;
        static syslog_msg_t queued_msg;
        va_list copy;
        va_copy(copy, args);
        int len = vsnprintf(queued_msg.text, SYSLOG_MSG_MAX, fmt, copy);
        va_end(copy);
        if (len > 0) {
            queued_msg.len = len < SYSLOG_MSG_MAX - 1 ? len : SYSLOG_MSG_MAX - 1;
            xQueueSend(s_queue, &queued_msg, 0);  // Drop if queue full
        }
        s_in_hook = false;
    }
    return s_original_vprintf(fmt, args);
}

void syslog_remote_init(const char *host, uint16_t port, const char *hostname,
                        syslog_format_t fmt, syslog_transport_t transport) {
    if (s_active) {
        syslog_remote_deinit();
    }

    s_format = fmt;
    s_transport = transport;
    strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';

    // Create message queue
    s_queue = xQueueCreate(SYSLOG_QUEUE_SIZE, sizeof(syslog_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return;
    }

    // Resolve server address
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *result = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int err = getaddrinfo(host, port_str, &hints, &result);
    if (err != 0 || result == NULL) {
        ESP_LOGE(TAG, "DNS resolution failed for %s: %d", host, err);
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }

    memcpy(&s_server_addr, result->ai_addr, sizeof(s_server_addr));
    s_server_addr.sin_port = htons(port);
    freeaddrinfo(result);

    ESP_LOGI(TAG, "Resolved %s to %d.%d.%d.%d",
             host,
             (s_server_addr.sin_addr.s_addr >> 0) & 0xFF,
             (s_server_addr.sin_addr.s_addr >> 8) & 0xFF,
             (s_server_addr.sin_addr.s_addr >> 16) & 0xFF,
             (s_server_addr.sin_addr.s_addr >> 24) & 0xFF);

    // Create socket
    if (transport == SYSLOG_TRANSPORT_TCP) {
        s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_sock >= 0) {
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(s_sock, (struct sockaddr *)&s_server_addr, sizeof(s_server_addr)) < 0) {
                ESP_LOGW(TAG, "TCP connect failed, will retry on first send");
                close(s_sock);
                s_sock = -1;
            }
        }
    } else {
        s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }

    if (s_sock < 0 && transport == SYSLOG_TRANSPORT_UDP) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }

    // Start sender task
    s_active = true;
    xTaskCreate(syslog_sender_task, "syslog_tx", SYSLOG_TASK_STACK, NULL, 2, &s_task);

    // Install vprintf hook
    s_original_vprintf = esp_log_set_vprintf(syslog_vprintf_hook);

    ESP_LOGI(TAG, "Remote syslog started: %s:%u (%s, %s)",
             host, port,
             fmt == SYSLOG_FMT_RFC5424 ? "RFC5424" : "RFC3164",
             transport == SYSLOG_TRANSPORT_TCP ? "TCP" : "UDP");
}

void syslog_remote_deinit(void) {
    if (!s_active && !s_original_vprintf) return;

    // Disable forwarding first
    s_active = false;

    // Restore original vprintf
    if (s_original_vprintf) {
        esp_log_set_vprintf(s_original_vprintf);
        s_original_vprintf = NULL;
    }

    // Wait for sender task to exit
    if (s_task) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_task = NULL;
    }

    // Close socket
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    // Delete queue
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    s_hostname[0] = '\0';
    s_in_hook = false;
}

bool syslog_remote_is_active(void) {
    return s_active;
}
