#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "syscall.h"
#include "bearssl.h"
#include "https_ta.h"

static void say(const char* s) {
    write(1, s, strlen(s));
}

static void say_ssl_error(const char* where, br_ssl_client_context* sc,
    br_x509_minimal_context* xc)
{
    fprintf(stderr, "%s: ssl=%d x509=%d\n", where,
        br_ssl_engine_last_error(&sc->eng), xc ? xc->err : 0);
}

static int parse_ipv4(const char* s, unsigned long* out) {
    unsigned long parts[4];
    int count = 0;
    const char* p = s;
    if (!s || !out) return -1;
    while (*p && count < 4) {
        char* end = 0;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p || v > 255) return -1;
        parts[count++] = v;
        if (*end == '.') p = end + 1;
        else if (*end == '\0') p = end;
        else return -1;
    }
    if (count != 4 || *p != '\0') return -1;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

static int sock_read(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    for (;;) {
        ssize_t rlen = read(fd, buf, len);
        if (rlen <= 0) return -1;
        return (int)rlen;
    }
}

static int sock_write(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    for (;;) {
        ssize_t wlen = write(fd, buf, len);
        if (wlen <= 0) return -1;
        return (int)wlen;
    }
}

static void set_x509_time_now(br_x509_minimal_context* xc) {
    struct timeval tv;
    uint32_t days;
    uint32_t seconds;
    if (!xc) return;
    if (gettimeofday(&tv, 0) < 0 || tv.tv_sec < 0) {
        br_x509_minimal_set_time(xc, 739330U, 0U);
        return;
    }
    days = 719528U + (uint32_t)((uint64_t)tv.tv_sec / 86400ULL);
    seconds = (uint32_t)((uint64_t)tv.tv_sec % 86400ULL);
    br_x509_minimal_set_time(xc, days, seconds);
}

int main(int argc, char** argv) {
    int fd;
    unsigned long ip = 0;
    int port = 443;
    const char* host = "example.com";
    const char* path = "/";
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    unsigned char seed[64];

    if (argc >= 2) host = argv[1];
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            say("httpsfetch: bad port\n");
            return 1;
        }
    }
    if (argc >= 4) path = argv[3];

    if (parse_ipv4(host, &ip) < 0) {
        if (dns_lookup_ipv4(host, (uint32_t*)&ip) < 0) {
            say("httpsfetch: bad host\n");
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        say("httpsfetch: socket failed\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = (unsigned short)(((unsigned)port >> 8) | ((unsigned)port << 8));
    addr.sin_addr.s_addr = (in_addr_t)ip;

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        say("httpsfetch: connect failed\n");
        close(fd);
        return 1;
    }

    if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        say("httpsfetch: getrandom failed\n");
        close(fd);
        return 1;
    }

    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    set_x509_time_now(&xc);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));

    if (!br_ssl_client_reset(&sc, host, 0)) {
        say("httpsfetch: tls reset failed\n");
        close(fd);
        return 1;
    }

    br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
    br_sslio_write_all(&ioc, "GET ", 4);
    br_sslio_write_all(&ioc, path, strlen(path));
    br_sslio_write_all(&ioc, " HTTP/1.0\r\nHost: ", 17);
    br_sslio_write_all(&ioc, host, strlen(host));
    br_sslio_write_all(&ioc, "\r\nConnection: close\r\n\r\n", 21);
    if (br_sslio_flush(&ioc) < 0) {
        say("httpsfetch: tls write failed\n");
        say_ssl_error("httpsfetch: flush", &sc, &xc);
        close(fd);
        return 1;
    }

    say("httpsfetch: response begin\n");
    for (;;) {
        unsigned char buf[512];
        int rlen = br_sslio_read(&ioc, buf, sizeof(buf));
        if (rlen < 0) {
            say_ssl_error("httpsfetch: read", &sc, &xc);
            break;
        }
        if (rlen == 0) break;
        write(1, buf, (size_t)rlen);
    }
    say("\nhttpsfetch: response end\n");

    close(fd);
    if (br_ssl_engine_current_state(&sc.eng) == BR_SSL_CLOSED
        && br_ssl_engine_last_error(&sc.eng) != 0) {
        fprintf(stderr, "httpsfetch: ssl error %d\n",
            br_ssl_engine_last_error(&sc.eng));
        return 1;
    }
    return 0;
}
