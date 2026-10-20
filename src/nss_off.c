/* AmiStudio Host - static release build only (Host 0.1.10): glibc's NSS lookups are switched off.
 * In a statically linked glibc program, getaddrinfo/gethostbyname/getpwnam & co. dlopen() the system's NSS modules
 * (/etc/nsswitch.conf: libnss_myhostname, libnss_resolve, mdns, sss...), and those load the system's own libc.so.6.
 * A libc of another glibc version crashes in __libc_early_init (Fedora 44 / Nobara: SIGFPE, Host 0.1.9 died at
 * "Sign in..."). The Makefile links the static binary with -Wl,--wrap=<each name below>, so every call, ours or a
 * library's (OpenSSL's BIO_lookup), lands here and fails cleanly, and glibc's NSS code is not linked at all.
 * The Host resolves names through the system's `getent` instead (plat_posix.c plat_resolve). */
#include <errno.h>
#include <netdb.h>
#include <pwd.h>
#include <grp.h>
#include <stddef.h>

int __wrap_getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r) { (void)n; (void)s; (void)h; if (r) *r = NULL; return EAI_FAIL; }
void __wrap_freeaddrinfo(struct addrinfo *r) { (void)r; }
int __wrap_getnameinfo(const void *sa, unsigned sl, char *h, unsigned hl, char *s, unsigned sv, int f) { (void)sa; (void)sl; (void)h; (void)hl; (void)s; (void)sv; (void)f; return EAI_FAIL; }
struct hostent *__wrap_gethostbyname(const char *n) { (void)n; h_errno = NO_RECOVERY; return NULL; }
struct hostent *__wrap_gethostbyname2(const char *n, int af) { (void)n; (void)af; h_errno = NO_RECOVERY; return NULL; }
struct hostent *__wrap_gethostbyaddr(const void *a, unsigned l, int t) { (void)a; (void)l; (void)t; h_errno = NO_RECOVERY; return NULL; }
int __wrap_gethostbyname_r(const char *n, struct hostent *h, char *b, size_t bl, struct hostent **r, int *e) { (void)n; (void)h; (void)b; (void)bl; if (r) *r = NULL; if (e) *e = NO_RECOVERY; return ENOENT; }
int __wrap_gethostbyname2_r(const char *n, int af, struct hostent *h, char *b, size_t bl, struct hostent **r, int *e) { (void)n; (void)af; (void)h; (void)b; (void)bl; if (r) *r = NULL; if (e) *e = NO_RECOVERY; return ENOENT; }
struct servent *__wrap_getservbyname(const char *n, const char *p) { (void)n; (void)p; return NULL; }
struct servent *__wrap_getservbyport(int port, const char *p) { (void)port; (void)p; return NULL; }
struct protoent *__wrap_getprotobyname(const char *n) { (void)n; return NULL; }
struct passwd *__wrap_getpwnam(const char *n) { (void)n; errno = ENOENT; return NULL; }
struct passwd *__wrap_getpwuid(unsigned u) { (void)u; errno = ENOENT; return NULL; }
int __wrap_getpwnam_r(const char *n, struct passwd *p, char *b, size_t bl, struct passwd **r) { (void)n; (void)p; (void)b; (void)bl; if (r) *r = NULL; return ENOENT; }
int __wrap_getpwuid_r(unsigned u, struct passwd *p, char *b, size_t bl, struct passwd **r) { (void)u; (void)p; (void)b; (void)bl; if (r) *r = NULL; return ENOENT; }
struct group *__wrap_getgrnam(const char *n) { (void)n; errno = ENOENT; return NULL; }
struct group *__wrap_getgrgid(unsigned g) { (void)g; errno = ENOENT; return NULL; }
