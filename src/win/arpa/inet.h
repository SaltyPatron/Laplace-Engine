/* Byte order on Windows (little-endian x86-64), without Winsock: what the Engine takes from <arpa/inet.h>. */
#ifndef LAPLACE_WIN_ARPA_INET_H
#define LAPLACE_WIN_ARPA_INET_H
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t htonl(uint32_t x){ return _byteswap_ulong(x); }
static inline uint32_t ntohl(uint32_t x){ return _byteswap_ulong(x); }
static inline uint16_t htons(uint16_t x){ return _byteswap_ushort(x); }
static inline uint16_t ntohs(uint16_t x){ return _byteswap_ushort(x); }
#endif
