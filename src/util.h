/* AmiStudio Host - small helpers: logging, framing, hex, SHA-256 (via OpenSSL), constant-time compare. */
#ifndef ASH_UTIL_H
#define ASH_UTIL_H
#include "plat.h"
#include <stdint.h>
#include <stddef.h>
extern int g_verbose;
void  logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (g_verbose) logf_(__VA_ARGS__); } while (0)
void     put_u16(unsigned char *p, unsigned v);
void     put_u32(unsigned char *p, uint32_t v);
unsigned get_u16(const unsigned char *p);
uint32_t get_u32(const unsigned char *p);
/* frames: u32le len (op + payload), u8 op, payload */
int  frame_send(sock_t s, int op, const void *p, uint32_t n);
/* reads one frame; *buf is malloc'ed (caller frees), payload length in *n. 0 ok, -1 error/oversize, -2 closed */
int  frame_recv(sock_t s, int *op, unsigned char **buf, uint32_t *n, uint32_t max);
void hex(const unsigned char *b, int n, char *out);           /* out: 2n+1 */
int  ct_equal(const char *a, const char *b);                   /* constant time for equal lengths */
void sha256_hex(const void *b, size_t n, char out[65]);
void trim(char *s);
#endif
