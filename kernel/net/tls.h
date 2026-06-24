#ifndef ASD_TLS_H
#define ASD_TLS_H

#include <stdint.h>

/* Initialise TLS subsystem — call once after net_init() */
void net_tls_init(void);

/* Perform TLS 1.3 handshake over an existing TCP connection.
 * tcp_id : result of net_tcp_connect()
 * hostname: used for SNI; also used for logging (no cert verification in v1)
 * Returns TLS connection slot (≥0) on success, -1 on error. */
int  net_tls_connect(int tcp_id, const char *hostname);

/* Send data over an established TLS connection.
 * Returns bytes sent or -1 on error. */
int  net_tls_send(int id, const void *data, uint32_t len);

/* Receive data from a TLS connection.
 * blocking=1: waits for data.
 * Returns bytes read, 0 on close, -1 on error. */
int  net_tls_recv(int id, void *buf, uint32_t cap, int blocking);

/* Close a TLS connection (does not close the underlying TCP). */
void net_tls_close(int id);

#endif /* ASD_TLS_H */
