/* autopatch60-oneshot.c -- one-shot variant of the resident daemon.
 * Waits for the RDR2 eboot (max. 10 min), verifies, writes, re-verifies,
 * notifies and EXITS. No residency: after notifying, neither process
 * nor traffic remains. Same safety: it never writes without verifying
 * (64-byte context + original bytes). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PACKET_MAGIC  0xFFAABBCCu
#define WIRE_SUCCESS  0x80000000u

#define CMD_PROC_READ      0xBDAA0002u
#define CMD_PROC_WRITE     0xBDAA0003u
#define CMD_PROC_MAPS_X    0xBDAA0004u
#define CMD_CONSOLE_NOTIFY 0xBDDD0004u
#define CMD_FG_APP         0xBDDD0006u

#define PROT_EXEC 0x4u
#define IMG_BASE_DEFAULT 0x400000u     /* base seen 100% of the time; fast-path only */
#define CTX_OFF 32

/* patch-site table: one row per verified game version. Memory decides which
 * row applies (64-byte context gate); app_ver is only used for logging.
 * orig: cmove esi,eax ; patch: xor esi,esi ; nop  (forces first display mode) */
struct patch_row {
    uint32_t rva;               /* target RVA (XML Address - 0x400000) */
    uint8_t plen;               /* patch length in bytes */
    const uint8_t *orig;
    const uint8_t *patch;
    const uint8_t *ctx;         /* 64 B, target at +32 */
    const char *ver;
};

static const uint8_t ORIG3[3] = {0x0f, 0x44, 0xf0}; /* cmove esi,eax */
static const uint8_t NEW3[3]  = {0x31, 0xf6, 0x90}; /* xor esi,esi;nop */

/* v1.00 context (scene base pkg; unique anchor match; disasm verified) */
static const uint8_t CTX_100[64] = {
    0xff,0x0f,0x82,0xea,0xfd,0xff,0xff,0x4c,0x8b,0xb5,0x78,0xfe,0xff,0xff,0x8b,0x3d,
    0x25,0xca,0xa8,0x03,0x41,0x8b,0x86,0xb8,0x00,0x00,0x00,0x8d,0x70,0xff,0x85,0xc0,
    0x0f,0x44,0xf0,0xe8,0xc9,0x55,0x3e,0x01,0x85,0xc0,0x0f,0x88,0x30,0x0a,0x00,0x00,
    0x45,0x8b,0x86,0x94,0x00,0x00,0x00,0x45,0x8b,0x8e,0x98,0x00,0x00,0x00,0x48,0x8d
};

/* v1.29 context (update installed on console; unique anchor match; same cmove).
 * illusion's public address 0x0578AB57 points at the `test eax,eax` before the
 * cmove (5-byte overwrite of test+cmove); we patch the 3-byte cmove only. */
static const uint8_t CTX_129[64] = {
    0x89,0x04,0xe9,0x49,0xff,0xc5,0x4d,0x39,0xf5,0x0f,0x82,0xe8,0xfd,0xff,0xff,0x8b,
    0x05,0x02,0x9e,0xc6,0x03,0x8b,0x3d,0x0c,0xa8,0xc7,0x03,0x8d,0x70,0xff,0x85,0xc0,
    0x0f,0x44,0xf0,0xe8,0x1f,0xd6,0x62,0x01,0x85,0xc0,0x0f,0x88,0xac,0x10,0x00,0x00,
    0x44,0x8b,0x05,0xb8,0x9d,0xc6,0x03,0x44,0x8b,0x0d,0xb5,0x9d,0xc6,0x03,0x48,0x8d
};

/* v1.32 context (Jao; verified against running clean 1.32) */
static const uint8_t CTX_132[64] = {
    0x89,0x04,0xe9,0x49,0xff,0xc5,0x4d,0x39,0xf5,0x0f,0x82,0xe8,0xfd,0xff,0xff,0x8b,
    0x05,0xb2,0xcf,0xd5,0x03,0x8b,0x3d,0xcc,0xd9,0xd6,0x03,0x8d,0x70,0xff,0x85,0xc0,
    0x0f,0x44,0xf0,0xe8,0x2f,0xad,0x70,0x01,0x85,0xc0,0x0f,0x88,0xac,0x10,0x00,0x00,
    0x44,0x8b,0x05,0x68,0xcf,0xd5,0x03,0x44,0x8b,0x0d,0x65,0xcf,0xd5,0x03,0x48,0x8d
};

#define NROWS 3
static const struct patch_row ROWS[NROWS] = {
    { 0x468EE1Fu, 3, ORIG3, NEW3, CTX_100, "01.00" },
    { 0x538AB59u, 3, ORIG3, NEW3, CTX_129, "01.29" },
    { 0x5453029u, 3, ORIG3, NEW3, CTX_132, "01.32" },
};

/* USB telemetry (read back later over FTP). Nothing sensitive. */
#define LOG_PATH "/mnt/usb0/autopatch60.log"
static unsigned log_n = 0;

static void tlog(const char *msg) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    fprintf(f, "%u %s\n", log_n++, msg);
    fclose(f);
}

static const char WANT_TITLEID[] = "CUSA03041";

static int g_sock = -1;

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put64le(uint8_t *p, uint64_t v) {
    put32le(p, (uint32_t)v); put32le(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64le(const uint8_t *p) {
    return get32le(p) | ((uint64_t)get32le(p + 4) << 32);
}

/* read exactly n bytes or fail */
static int recvn(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(g_sock, (char *)buf + got, n - got, 0);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int sendn(const uint8_t *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(g_sock, (const char *)buf + sent, n - sent, 0);
        if (r <= 0)
            return -1;
        sent += (size_t)r;
    }
    return 0;
}

static int cmd(uint32_t id, const uint8_t *body, uint32_t len) {
    uint8_t h[12];
    put32le(h, PACKET_MAGIC); put32le(h + 4, id); put32le(h + 8, len);
    if (sendn(h, 12))
        return -1;
    if (len && sendn(body, len))
        return -1;
    return 0;
}

static int expect_success(void) {
    uint8_t b[4];
    if (recvn(b, 4))
        return -1;
    return get32le(b) == WIRE_SUCCESS ? 0 : -1;
}

static int connect_dbg(void) {
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0)
        return -1;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(744);
    sa.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa))) {
        close(g_sock);
        g_sock = -1;
        return -1;
    }
    return 0;
}

static int notify(const char *msg) {
    uint32_t len = (uint32_t)strlen(msg) + 1;
    uint8_t b[8];
    char lb[128];
    int rc;
    put32le(b, 0); put32le(b + 4, len);
    if (cmd(CMD_CONSOLE_NOTIFY, b, 8)) {
        tlog("NOTIFY_SEND_FAIL");
        return -1;
    }
    /* trailing data: text + NUL */
    if (sendn((const uint8_t *)msg, len)) {
        tlog("NOTIFY_DATA_FAIL");
        return -1;
    }
    rc = expect_success();
    snprintf(lb, sizeof(lb), "NOTIFY rc=%d msg=%.64s", rc, msg);
    tlog(lb);
    return rc;
}

/* foreground app: pid + titleid + app_ver (param.sfo, offset 124 of response) */
static int fg_app(uint32_t *pid, char titleid[16], char appver[16]) {
    uint8_t resp[140];
    if (cmd(CMD_FG_APP, NULL, 0))
        return -1;
    if (expect_success())
        return -1;
    if (recvn(resp, 140))
        return -1;
    *pid = get32le(resp);
    memcpy(titleid, resp + 4, 16);
    titleid[15] = 0;
    memcpy(appver, resp + 124, 16);
    appver[15] = 0;
    return 0;
}

static int proc_read(uint32_t pid, uint64_t addr, uint8_t *out, uint32_t len) {
    uint8_t b[16];
    put32le(b, pid); put64le(b + 4, addr); put32le(b + 12, len);
    if (cmd(CMD_PROC_READ, b, 16))
        return -1;
    if (expect_success())
        return -1;
    return recvn(out, len);
}

static int proc_write(uint32_t pid, uint64_t addr, const uint8_t *data, uint32_t len) {
    uint8_t b[16];
    put32le(b, pid); put64le(b + 4, addr); put32le(b + 12, len);
    if (cmd(CMD_PROC_WRITE, b, 16))
        return -1;
    if (expect_success())
        return -1;
    if (sendn(data, len))
        return -1;
    return expect_success();
}

/* local memmem in case the payload libc lacks it */
static void *local_memmem(const void *h, size_t hlen, const void *n, size_t nlen) {
    const uint8_t *hh = h;
    size_t i;
    if (!nlen || nlen > hlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (!memcmp(hh + i, n, nlen))
            return (void *)(hh + i);
    return NULL;
}

/* base = start of the lowest RX 'executable' segment (home of .text) */
static int eboot_base(uint32_t pid, uint64_t *base) {
    uint8_t b[4], hdr[8];
    uint64_t best = 0;
    uint32_t num, i;
    put32le(b, pid);
    if (cmd(CMD_PROC_MAPS_X, b, 4))
        return -1;
    if (expect_success())
        return -1;
    if (recvn(hdr, 4))
        return -1;
    num = get32le(hdr);
    for (i = 0; i < num; i++) {
        uint8_t e[58];
        uint64_t start, end, sz;
        uint16_t prot;
        if (recvn(e, 58))
            return -1;
        start = get64le(e + 32); end = get64le(e + 40);
        sz = end - start;
        prot = (uint16_t)(e[56] | (e[57] << 8));
        if (!(prot & PROT_EXEC))
            continue;
        if (local_memmem(e, 32, "executable", 10) == NULL)
            continue;
        if (sz < 0x1000000) /* eboot .text is tens of MB */
            continue;
        if (best == 0 || start < best)
            best = start;
    }
    if (!best)
        return -1;
    *base = best;
    return 0;
}

/* Try every table row at the given image base. Returns:
 * 1 = patched (or already patched) and notified; 2 = unexpected bytes (notified);
 * 3 = write failed (transient); 0 = no row's context matched. */
static int try_rows(uint32_t pid, uint64_t base) {
    char lb[160];
    int i;
    for (i = 0; i < NROWS; i++) {
        const struct patch_row *r = &ROWS[i];
        uint64_t target = base + r->rva;
        uint8_t ctx[64], cur[8];
        if (proc_read(pid, target - CTX_OFF, ctx, 64))
            continue;
        if (memcmp(ctx, r->ctx, 64) != 0)
            continue;
        memcpy(cur, ctx + CTX_OFF, 8);
        snprintf(lb, sizeof(lb), "ROW %s hit pid=%u", r->ver, pid);
        tlog(lb);
        if (!memcmp(cur, r->patch, r->plen)) {
            tlog("ALREADY");
            snprintf(lb, sizeof(lb), "RDR2 60fps ON v%s - By KurohaXR", r->ver);
            notify(lb);
            return 1;
        }
        if (memcmp(cur, r->orig, r->plen) != 0) {
            snprintf(lb, sizeof(lb), "ABORT_UNEXP %02x%02x%02x ver=%s",
                     cur[0], cur[1], cur[2], r->ver);
            tlog(lb);
            snprintf(lb, sizeof(lb), "RDR2: unexpected eboot v%s, not applied - By KurohaXR",
                     r->ver);
            notify(lb);
            return 2;
        }
        tlog("WRITE_TRY");
        if (proc_write(pid, target, r->patch, r->plen) ||
            proc_read(pid, target, cur, r->plen) ||
            memcmp(cur, r->patch, r->plen) != 0) {
            tlog("WRITE_FAIL");
            return 3;
        }
        tlog("WRITE_OK");
        snprintf(lb, sizeof(lb), "RDR2 60fps ON v%s - By KurohaXR", r->ver);
        notify(lb);
        return 1;
    }
    return 0;
}

/* one-shot flow: multi-version table with a 64-byte context gate per row.
 * Fast-path at the default image base (0x400000), MAPS fallback once.
 * Poll every 0.5 s while waiting for the game; exits after patching. */
#define POLL_FAST_USEC 500000u
#define AUTOPATCH_VERSION "oneshot-1.2"

int main(void) {
    /* Wait (max. 10 min) for RDR2 to boot, patch once verified and EXIT.
     * No residency: after notifying, no process or traffic remains.
     * If the game never shows up, exit quietly. */
    uint32_t pid = 0;
    char titleid[16] = {0};
    char appver[16] = {0};
    uint64_t base = 0;
    unsigned waited = 0;
    char lb[160];
    const unsigned LIMIT = 1200; /* 1200 x 0.5 s = 10 min */

    printf("autopatch60 v%s (one-shot): waiting for game...\n", AUTOPATCH_VERSION);
    tlog("BOOT oneshot " AUTOPATCH_VERSION);
    if (g_sock < 0 && connect_dbg()) {
        tlog("CONN_FAIL0");
        notify("ps5debug-NG not found or not running");
        return 2;
    }
    tlog("CONN_OK");
    notify("RDR2: waiting for game");
    /* 1) wait for spawn */
    for (waited = 0; waited < LIMIT; waited++) {
        if (fg_app(&pid, titleid, appver))
            break; /* dead connection: exit, nothing to patch */
        if (pid && !strcmp(titleid, WANT_TITLEID))
            break;
        usleep(POLL_FAST_USEC);
    }
    if (waited >= LIMIT || !pid || strcmp(titleid, WANT_TITLEID) != 0) {
        tlog("TIMEOUT_NO_GAME");
        return 0; /* quiet exit: nothing was ever launched */
    }
    snprintf(lb, sizeof(lb), "SPAWN pid=%u app_ver=%s", pid, appver);
    tlog(lb);
    /* 2) fast-path rows at the default image base */
    if (try_rows(pid, IMG_BASE_DEFAULT) == 1)
        return 0;
    /* 3) MAPS fallback: re-resolve base and retry once */
    if (!eboot_base(pid, &base)) {
        snprintf(lb, sizeof(lb), "BASE 0x%llx pid=%u",
                 (unsigned long long)base, pid);
        tlog(lb);
        if (try_rows(pid, base) == 1)
            return 0;
    }
    tlog("NO_TARGET");
    notify("RDR2: game version not supported - By KurohaXR");
    return 3;
}
