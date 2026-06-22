#ifndef ASD_VIRTIO_NET_H
#define ASD_VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>

void    virtio_net_init(void);
int     virtio_net_send(const void *frame, uint16_t len);
void    virtio_net_rx_poll(void);
void    virtio_net_get_mac(uint8_t mac[6]);
int     virtio_net_ready(void);
uint16_t virtio_net_rx_used_idx(void);

#endif
