/* ASH v1 - AmiStudio Host protocol (docs/PROTOCOL.md). Shared by the Host and by clients (client/ashc.c).
 * One TCP connection per service session on port 9220. After the handshake the socket belongs to the service. */
#ifndef ASH_H
#define ASH_H
#define ASH_MAGIC       "ASH1"
#define ASH_VERSION     1
#define ASH_PORT        9220
#define ASH_MAX_TOKEN   64
#define ASH_TOKEN_HEX   64            /* paired tokens: 32 random bytes as 64 lower-case hex chars */
#define ASH_MAX_NAME    32
#define ASH_MAX_CLIENT  64

/* handshake reply status */
enum { ASH_OK = 0, ASH_BAD_TOKEN = 1, ASH_NO_SERVICE = 2, ASH_TOO_OLD = 3, ASH_TOO_NEW = 4, ASH_BUSY = 5,
       ASH_REFUSED = 6 /* banned for a while after repeated failures, or service failed to start */ };

/* service framing used by the built-in services (and recommended for new ones): u32le len, then len bytes = u8 op + payload */
#define ASH_MAX_FRAME   (1024 * 1024)

/* hostinfo service (v1): host sends one INFO frame of "key value" text lines, then closes */
enum { HI_INFO = 1 };
/* pair service (v1): client PAIR_REQ "123456[\nclient name]"; host PAIR_OK "<64 hex token>" or PAIR_FAIL "<message>" */
enum { PAIR_REQ = 1, PAIR_OK = 2, PAIR_FAIL = 3 };
/* fetch service (v1): client GET "url[\nsize[\nsha256]]"; host HEAD (u32le size or 0xFFFFFFFF, u16le http status),
 * DATA chunks (<= 32 KB), then END ("<sha256 hex>" + u32le bytes) or ERR "<message>". Several GETs per connection. */
enum { FETCH_GET = 1, FETCH_HEAD = 2, FETCH_DATA = 3, FETCH_END = 4, FETCH_ERR = 5 };
#define FETCH_CHUNK     32768
/* control service (v1, Host 0.1.6+; paired or loopback): the launcher asks the Host to quit when nothing uses it.
 * client QUIT_WHEN_UNUSED [u16le grace seconds, 0/absent = 10] | QUIT_CANCEL | QUIT_STATUS (all may repeat);
 * host answers each with QUIT_STATE: u8 state (0 not armed, 1 armed: waits for the other clients, 2 stopping now),
 * u16le other client sessions, u16le grace seconds. */
enum { CTL_QUIT_WHEN_UNUSED = 1, CTL_QUIT_CANCEL = 2, CTL_QUIT_STATUS = 3, CTL_QUIT_STATE = 16 };
enum { CTL_NOT_ARMED = 0, CTL_ARMED = 1, CTL_STOPPING = 2 };
#define CTL_GRACE_DEFAULT 10
#define CTL_GRACE_MAX     300

/* amidrive service (v1, Host 0.1.8+; paired or loopback, max 4 clients): Google Drive mounted on the Host PC with rclone
 * (docs/PROTOCOL.md "amidrive v1"). Strings are u8 length + UTF-8 (cut to 255 bytes). STATUS and WATCH get one STATE; the other requests get a RESULT (to the requester only). */
enum { AD_STATUS = 1, AD_WATCH = 2, AD_AUTH_START = 3, AD_AUTH_CANCEL = 4, AD_CONNECT = 5, AD_UNMOUNT = 6, AD_FLUSH = 7, AD_SIGNOUT = 8,
       AD_STATE = 16, AD_AUTH = 17, AD_RESULT = 18, AD_PROGRESS = 19 };
enum { ADS_UNAVAILABLE = 0, ADS_SIGNED_OUT = 1, ADS_SIGNING_IN = 2, ADS_READY = 3, ADS_MOUNTING = 4, ADS_MOUNTED = 5,
       ADS_FLUSHING = 6, ADS_UNMOUNTING = 7, ADS_ERROR = 8 };
enum { ADF_FUSE = 1, ADF_RCLONE = 2, ADF_PENDING = 4, ADF_LOWDISK = 8, ADF_EXPIRED = 16, ADF_NOTEMPTY = 32, ADF_COPIED = 64 };
enum { ADR_OK = 0, ADR_FILES_OPEN = 1, ADR_FAILED = 2, ADR_NOT_SIGNED_IN = 3, ADR_BUSY = 4, ADR_PENDING = 5, ADR_EXPIRED = 6,
       ADR_NOT_EMPTY = 7, ADR_COPIED = 8 };
enum { ADA_URL = 0, ADA_OPENED = 1, ADA_DONE = 2, ADA_FAILED = 3 };
/* AUTH_START methods: 0 the desktop browser on the Host PC, 1 URL only, 2 (0.1.9+) URL for AmiSurf
   (always URL-only; AUTH 0's flags byte says whether the Host has a "browser" engine). Hosts up to 0.1.8 treat 2 as 0,
   so a client may always send 2 and open an AUTH 0 URL in AmiSurf. */
enum { AD_AUTH_DESKTOP = 0, AD_AUTH_URL_ONLY = 1, AD_AUTH_ENGINE = 2 };
#define AD_CONNECT_AUTO 1           /* CONNECT flags */
#define AD_SIGNOUT_REVOKE 1         /* SIGNOUT flags */

/* LAN discovery: UDP to port 9220, request "ASH?"; reply "ASH!" then "key=value\n" lines (name, ver, port, pair, id) */
#define ASH_DISC_Q      "ASH?"
#define ASH_DISC_A      "ASH!"
#endif
