/**
 * @file syslog_remote.h
 * @brief Remote syslog forwarding for ESP-IDF log output
 *
 * Intercepts ESP_LOG* calls via esp_log_set_vprintf() and forwards
 * formatted syslog messages to a remote server over UDP or TCP.
 * Supports RFC 3164 (BSD) and RFC 5424 (structured) syslog formats.
 */

#ifndef SYSLOG_REMOTE_H
#define SYSLOG_REMOTE_H

#include <stdint.h>
#include <stdbool.h>

/** Syslog message format */
typedef enum {
    SYSLOG_FMT_RFC3164 = 0,   /**< BSD syslog (RFC 3164) */
    SYSLOG_FMT_RFC5424 = 1    /**< Structured syslog (RFC 5424) */
} syslog_format_t;

/** Syslog transport protocol */
typedef enum {
    SYSLOG_TRANSPORT_UDP = 0,  /**< UDP (fire-and-forget) */
    SYSLOG_TRANSPORT_TCP = 1   /**< TCP (persistent connection, RFC 6587 framing) */
} syslog_transport_t;

/**
 * @brief Initialize remote syslog forwarding
 *
 * Resolves the server hostname, opens a socket, and installs the
 * vprintf hook to intercept all ESP_LOG* output. The original vprintf
 * (UART) is preserved and chained.
 *
 * IMPORTANT: Must be called from app_main() after WiFi is connected,
 * NOT from wifi_event_handler (getaddrinfo needs significant stack).
 *
 * @param host      Syslog server hostname or IP address
 * @param port      Syslog server port (typically 514)
 * @param hostname  Device hostname (used in syslog HOSTNAME field)
 * @param fmt       Message format (RFC 3164 or RFC 5424)
 * @param transport Transport protocol (UDP or TCP)
 */
void syslog_remote_init(const char *host, uint16_t port,
                        const char *hostname,
                        syslog_format_t fmt,
                        syslog_transport_t transport);

/**
 * @brief Shut down remote syslog forwarding
 *
 * Restores the original vprintf, closes the socket, and releases
 * the mutex. Safe to call even if init was never called.
 */
void syslog_remote_deinit(void);

/**
 * @brief Check if remote syslog is currently active
 * @return true if initialized and forwarding logs
 */
bool syslog_remote_is_active(void);

#endif /* SYSLOG_REMOTE_H */
