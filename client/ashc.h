/* ashc - tiny ASH v1 client for AmiStudio apps (portable C89-ish, BSD sockets; on the Amiga link with
 * bsdsocket.library via proto/socket.h and define ASHC_AMIGA). Used by `amistudio-hostd status`, the tests,
 * and meant to be copied into Amiga clients (launcher Host panel, later Browser/PDF/...). */
#ifndef ASHC_H
#define ASHC_H
#include "../src/ash.h"
int  ashc_init(void);
void ashc_set_timeout(int ms);   /* connect + each read/write give up after ms (0 = wait forever, the default). Not on Amiga. */   /* Windows: WSAStartup; elsewhere nothing. 0 ok */
/* connect + handshake. Returns a socket (>= 0) on ASH_OK; otherwise -1 and *status/msg tell why
 * (status -1 = could not connect / protocol error). ver_out may be NULL. */
long ashc_open(const char *host, int port, const char *service, unsigned ver, const char *token, const char *client,
               int *status, unsigned *ver_out, char *msg, int mn);
int  ashc_send(long s, int op, const void *p, unsigned long n);                     /* 0 ok */
/* read one frame into buf (cap bytes); returns payload length, -1 error, -2 closed, -3 too big. *op set */
long ashc_recv(long s, int *op, void *buf, unsigned long cap);
void ashc_close(long s);
/* convenience: hostinfo text into out; pair (code -> 64-hex token into tok). 0 ok, else -1 with msg */
int  ashc_hostinfo(const char *host, int port, const char *token, char *out, int on, char *msg, int mn);
int  ashc_pair(const char *host, int port, const char *code, const char *name, char *tok, char *msg, int mn);
/* LAN discovery: send "ASH?" to addr (NULL = 255.255.255.255 broadcast) and collect replies for wait_ms.
 * Calls cb(addr, text, ud) per answer; returns the number of answers. */
int  ashc_discover(const char *addr, int port, int wait_ms, void (*cb)(const char *addr, const char *text, void *ud), void *ud);
#endif
