#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"

#include "lwip/dns.h"
#include "lwip/ip.h"
#include "lwip/prot/dns.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DIG_TIME_DIFF_MS(_end, _start) ((uint32_t)(((_end)->tv_sec - (_start)->tv_sec) * 1000 + \
                                                    ((_end)->tv_usec - (_start)->tv_usec) / 1000))

/* lwIP names every RR type this file cares about except the EDNS pseudo-record, which a
 * server may put in ADDITIONAL whether or not we asked for EDNS. Naming it keeps that
 * section legible instead of printing a blank type. */
#define DIG_RRTYPE_OPT 41

typedef struct {
    uint16_t type;
    const char *type_name;
} name_mapping_t;

static name_mapping_t rr_types[] = {
    { DNS_RRTYPE_A, "A" },
    { DNS_RRTYPE_NS, "NS" },
    { DNS_RRTYPE_MD, "MD" },
    { DNS_RRTYPE_MF, "MF" },
    { DNS_RRTYPE_CNAME, "CNAME" },
    { DNS_RRTYPE_SOA, "SOA" },
    { DNS_RRTYPE_MB, "MB" },
    { DNS_RRTYPE_MG, "MG" },
    { DNS_RRTYPE_MR, "MR" },
    { DNS_RRTYPE_NULL, "NULL" },
    { DNS_RRTYPE_WKS, "WKS" },
    { DNS_RRTYPE_PTR, "PTR" },
    { DNS_RRTYPE_HINFO, "HINFO" },
    { DNS_RRTYPE_MINFO, "MINFO" },
    { DNS_RRTYPE_MX, "MX" },
    { DNS_RRTYPE_TXT, "TXT" },
    { DNS_RRTYPE_AAAA, "AAAA" },
    { DNS_RRTYPE_SRV, "SRV" },
    { DIG_RRTYPE_OPT, "OPT" },
    { DNS_RRTYPE_ANY, "ANY" }
};

static name_mapping_t rr_class[] = {
    { DNS_RRCLASS_IN, "IN" },
    { DNS_RRCLASS_CS, "CS" },
    { DNS_RRCLASS_CH, "CH" },
    { DNS_RRCLASS_HS, "HS" },
    { DNS_RRCLASS_ANY, "ANY" }
};

/* Both of these used to be half-checking IPv4 patterns, which meant -b rejected a
 * perfectly good IPv6 source and -x refused to reverse one. They now do what the @server
 * pattern below already did: accept a token and let ipaddr_aton() be the judge, so the
 * error message names the actual problem instead of "illegal value". */
static const char *BIND_REGEX = "^[^ ]+$";
/* Longest alternative first, and anchored. argtable3's regex engine takes the first
 * alternative that matches at the start and does not come back for a longer one, so with
 * "A" leading, "AAAA" matched the initial "A", failed the end-of-input check and was
 * rejected as an illegal value. */
static const char *TYPE_REGEX = "^(AAAA|A|CNAME|MX|NS|PTR|SOA|SRV|TXT)$";
static const char *REVERSE_REGEX = "^[^ ]+$";

static const char *IP_REGEX = "^@([^ ]+)$";

/* Scratch for the name currently being rendered. Every use is print-and-discard within one
 * call, so one buffer is enough -- but it is file-local, not a global symbol. */
static char out[256];

static struct {
    struct arg_rex *server;
    struct arg_rex *bind;
    struct arg_int *port;
    struct arg_rex *t_type;
    struct arg_str *q_name;
    struct arg_rex *reverse;
    struct arg_lit *ipv4;
    struct arg_lit *ipv6;

    struct arg_str *name;
    struct arg_rex *type;
    // Command
    struct arg_end *end;
} dig_args;

static const char *dns_rr_type(const uint16_t type) {
    for (size_t idx = 0; idx < sizeof(rr_types) / sizeof(name_mapping_t); idx++) {
        if (rr_types[idx].type == type) {
            return rr_types[idx].type_name;
        }
    }

    return  "";
}

static const char *dns_rr_class(const uint16_t cls) {
    for (size_t idx = 0; idx < sizeof(rr_class) / sizeof(name_mapping_t); idx++) {
        if (rr_class[idx].type == cls) {
            return rr_class[idx].type_name;
        }
    }

    return  "";
}

/* The header's four-bit response code, which the header line used to print as a literal
 * "??" -- so an NXDOMAIN and a REFUSED were indistinguishable from a good answer with no
 * records. Returns NULL for a code with no assigned name; the caller prints the number. */
static const char *dns_rcode_name(uint8_t rcode)
{
    static const char *const names[] = {
        "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED",
        "YXDOMAIN", "YXRRSET", "NXRRSET", "NOTAUTH", "NOTZONE",
    };
    if (rcode < sizeof(names) / sizeof(names[0])) {
        return names[rcode];
    }
    return NULL;
}

/* Byte-wise, because rdata sits at whatever offset the preceding names left it at. */
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A reply is network input parsed on the console task's stack. Every walk below is bounded
 * by this; without it a truncated or hostile datagram reads off the end of rx()'s buffer. */
static bool dns_in_msg(const uint8_t *msg, const uint8_t *msg_end, const uint8_t *p, size_t need)
{
    return p >= msg && p <= msg_end && (size_t)(msg_end - p) >= need;
}

/**
 * @brief Render the domain name at `ptr` into `dst`, following compression pointers.
 *
 * @return bytes consumed AT `ptr` -- two if the name is or ends in a pointer -- or -1.
 *
 * Two things the original could not do. It only handled a pointer as the whole name, so a
 * name encoded as labels followed by a pointer, which is the common case in an answer,
 * decoded as garbage. And it took the pointer offset from the low byte alone, dropping the
 * six high bits, so any name pointing past offset 255 -- routine in a reply carrying an
 * SOA -- landed somewhere else entirely.
 */
static ssize_t dns_get_label(const uint8_t *msg, const uint8_t *msg_end, const uint8_t *ptr,
                             char *dst, size_t dst_size)
{
    const uint8_t *start = ptr;
    size_t written = 0;
    int hops = 0;
    ssize_t consumed = -1; /* fixed at the first pointer taken */

    for (;;) {
        if (!dns_in_msg(msg, msg_end, ptr, 1)) {
            return -1;
        }
        const uint8_t len = *ptr;

        if ((len & 0xc0) == 0xc0) {
            if (!dns_in_msg(msg, msg_end, ptr, 2)) {
                return -1;
            }
            if (consumed < 0) {
                consumed = (ptr + 2) - start;
            }
            if (++hops > 16) {
                return -1; /* a pointer loop */
            }
            const size_t off = (size_t)(((len & 0x3f) << 8) | ptr[1]);
            if (off >= (size_t)(msg_end - msg)) {
                return -1;
            }
            ptr = msg + off;
            continue;
        }
        if ((len & 0xc0) != 0) {
            return -1; /* reserved label type */
        }
        if (len == 0) {
            ptr++;
            break;
        }
        if (!dns_in_msg(msg, msg_end, ptr, (size_t)len + 1) || written + len + 2 > dst_size) {
            return -1;
        }
        memcpy(dst + written, ptr + 1, len);
        written += len;
        dst[written++] = '.';
        ptr += 1 + len;
    }

    if (written == 0) {
        if (dst_size < 2) {
            return -1;
        }
        dst[written++] = '.'; /* the root */
    }
    dst[written] = '\0';
    return (consumed >= 0) ? consumed : (ptr - start);
}

static size_t dns_get_question_length(const char *name)
{
    // The label length is the string length + 1
    size_t name_len = strlen(name) + 1;

    // Total length is label length aligned to 16 bits and then 2 16-bit integers
    return name_len + (sizeof(uint16_t) * 2);
}

static size_t dns_build_question(const char *q_name, uint16_t q_type, uint16_t q_class, uint8_t *question, size_t question_max_len)
{
    // Question Section
    //                                 1  1  1  1  1  1
    //   0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
    // +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    // |                                               |
    // /                     QNAME                     /
    // /                                               /
    // +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    // |                     QTYPE                     |
    // +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    // |                     QCLASS                    |
    // +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+

    // Total length is label length aligned to 16 bits and then 2 16-bit integers
    size_t q_len = dns_get_question_length(q_name);

    // Fail quickly if there isn't sufficient room to fill in the question
    if (q_len > question_max_len) {
        return 0;
    }

    // Start by building the label out
    uint8_t *q_ptr = question;
    const char *label_ptr = q_name;
    const char *label_end = q_name + strlen(q_name);
    while (label_ptr < label_end) {
        // Find The End
        const char *end_ptr = strchr(label_ptr, '.');
        if (NULL == end_ptr) {
            break;
        }

        size_t label_len = end_ptr - label_ptr;
        if (label_len > UINT8_MAX) {
            goto error;
        }

        *(q_ptr++) = (uint8_t)label_len;
        memcpy(q_ptr, label_ptr, label_len);
        q_ptr += label_len;

        // Now To Start To Process
        label_ptr = end_ptr;
        label_ptr++;
    }

    // Now Add A Zero Length
    *(q_ptr++) = 0; // A Zero Length Termination

    // then put QTYPE
    uint16_t type = htons(q_type);
    *(q_ptr++) = (uint8_t)(type & 0xff);
    *(q_ptr++) = (uint8_t)(type >> 8);

    // then put QCLASS
    uint16_t class = htons(q_class);
    *(q_ptr++) = (uint8_t)(class & 0xff);
    *(q_ptr++) = (uint8_t)(class >> 8);

    return q_len;
error:
    return 0;
}

static ssize_t dns_print_rr(const uint8_t *msg, const uint8_t *msg_end, const uint8_t *ptr)
{
    const uint8_t *start_ptr = ptr;

    // handle qname
    ssize_t len = dns_get_label(msg, msg_end, ptr, out, sizeof(out));
    if (len < 0) {
        return -1;
    }
    ptr += len;

    /* type, class, TTL, rdlength */
    if (!dns_in_msg(msg, msg_end, ptr, 10)) {
        return -1;
    }
    uint16_t type = rd_u16(ptr);
    ptr += 2;
    uint16_t class = rd_u16(ptr);
    ptr += 2;
    uint32_t ttl = rd_u32(ptr);
    ptr += 4;
    uint16_t rd_length = rd_u16(ptr);
    ptr += 2;

    if (!dns_in_msg(msg, msg_end, ptr, rd_length)) {
        return -1;
    }

    printf("%s\t%"PRIu32"\t%s\t%s", out, ttl, dns_rr_class(class), dns_rr_type(type));

    /* Only PTR used to print its rdata, so an A lookup -- the common case -- rendered as
     * "example.com. 300 IN A" with the address missing entirely. */
    switch (type)
    {
        case DNS_RRTYPE_A:
            if (rd_length == 4) {
                printf("\t%u.%u.%u.%u", ptr[0], ptr[1], ptr[2], ptr[3]);
            }
            break;
#if LWIP_IPV6
        case DNS_RRTYPE_AAAA:
            if (rd_length == 16) {
                struct in6_addr a6;
                memcpy(&a6, ptr, sizeof(a6));
                /* lwIP renders IPv6 in uppercase; RFC 5952 and dig itself use lowercase,
                 * and cmd_ip.c already lowercases for the same reason. */
                char a6_str[46];
                snprintf(a6_str, sizeof(a6_str), "%s", inet6_ntoa(a6));
                for (char *c = a6_str; *c != '\0'; c++) {
                    *c = (char)tolower((unsigned char)*c);
                }
                printf("\t%s", a6_str);
            }
            break;
#endif
        case DNS_RRTYPE_PTR:
        case DNS_RRTYPE_CNAME:
        case DNS_RRTYPE_NS:
            /* All three carry a bare domain name, compression and all. */
            if (dns_get_label(msg, msg_end, ptr, out, sizeof(out)) >= 0) {
                printf("\t%s", out);
            }
            break;
        case DNS_RRTYPE_MX:
            if (rd_length > 2) {
                printf("\t%u", (unsigned)rd_u16(ptr));
                if (dns_get_label(msg, msg_end, ptr + 2, out, sizeof(out)) >= 0) {
                    printf(" %s", out);
                }
            }
            break;
        case DNS_RRTYPE_SOA:
        {
            /* MNAME RNAME SERIAL REFRESH RETRY EXPIRE MINIMUM. Worth having even though a
             * query rarely asks for one: the SOA is what a server puts in the AUTHORITY
             * section of an NXDOMAIN, which is the reply you most want to be able to read. */
            const uint8_t *p = ptr;
            ssize_t n = dns_get_label(msg, msg_end, p, out, sizeof(out));
            if (n < 0) {
                break;
            }
            printf("\t%s", out);
            p += n;
            n = dns_get_label(msg, msg_end, p, out, sizeof(out));
            if (n < 0) {
                break;
            }
            printf(" %s", out);
            p += n;
            if ((size_t)(p - ptr) + 20 <= rd_length) {
                for (int i = 0; i < 5; i++) {
                    printf(" %"PRIu32, rd_u32(p));
                    p += 4;
                }
            }
        }
            break;
        case DNS_RRTYPE_SRV:
            /* PRIORITY WEIGHT PORT TARGET */
            if (rd_length > 6) {
                printf("\t%u %u %u", (unsigned)rd_u16(ptr), (unsigned)rd_u16(ptr + 2),
                       (unsigned)rd_u16(ptr + 4));
                if (dns_get_label(msg, msg_end, ptr + 6, out, sizeof(out)) >= 0) {
                    printf(" %s", out);
                }
            }
            break;
        case DNS_RRTYPE_TXT:
        {
            /* One or more length-prefixed strings, each quoted the way dig prints them. */
            const uint8_t *txt = ptr;
            const uint8_t *txt_end = ptr + rd_length;
            while (txt < txt_end && (txt + 1 + *txt) <= txt_end) {
                printf("\t\"%.*s\"", (int)*txt, (const char *)(txt + 1));
                txt += 1 + *txt;
            }
        }
            break;
        default:
            /* Better than printing nothing at all for a type this does not decode. */
            printf("\t(%u bytes)", (unsigned)rd_length);
            break;
    }
    printf("\n");

    // Advance The Pointer
    ptr += rd_length;

    return ptr - start_ptr;
}

/* Returns false as soon as a record does not parse, so a malformed reply stops the section
 * rather than running the pointer on from a bad length. */
static bool print_section(const char *title, const uint8_t *msg, const uint8_t *msg_end,
                          const uint8_t **ptr, uint16_t count)
{
    if (count == 0) {
        return true;
    }
    printf(";; %s SECTION:\n", title);
    for (uint16_t idx = 0; idx < count; idx++) {
        ssize_t used = dns_print_rr(msg, msg_end, *ptr);
        if (used < 0) {
            printf(";; (truncated or malformed after %u record%s)\n",
                   (unsigned)idx, idx == 1 ? "" : "s");
            return false;
        }
        *ptr += used;
    }
    printf("\n");
    return true;
}

static ssize_t send_request(int fd, const struct sockaddr *addr, const socklen_t addr_len,
                            const char *hostname, uint16_t q_type, struct timeval *start_time)
{
    /* +---------------------+
     * |        Header       |
     * +---------------------+
     * |       Question      | the question for the name server
     * +---------------------+
     * |        Answer       | RRs answering the question
     * +---------------------+
     * |      Authority      | RRs pointing toward an authority
     * +---------------------+
     * |      Additional     | RRs holding additional information
     * +---------------------+
     */
    size_t q_len = dns_get_question_length(hostname);

    size_t length = SIZEOF_DNS_HDR + q_len;
    uint8_t *payload = mem_calloc(length, sizeof(uint8_t));
    if (payload == NULL) {
        printf("payload == NULL\n");
        return 0;
    }

    // Populate The Header
    struct dns_hdr *hdr = (struct dns_hdr *)payload;
    hdr->id = htons(1);
    hdr->flags1 = DNS_FLAG1_RD;    // Query with Recursive
    hdr->flags2 = 0x20; // AD: we accept the server's own validation verdict
    hdr->numquestions = htons(1);

    // Populate The Question Section
    uint8_t *ptr = payload + SIZEOF_DNS_HDR;
    size_t built = dns_build_question(hostname, q_type, DNS_RRCLASS_IN, ptr, q_len);
    if (built == 0) {
        printf("dig: cannot encode question for %s\n", hostname);
        mem_free(payload);
        return -1;
    }

    // Send The Message & Capture The Time
    ssize_t sent = sendto(fd, payload, length, 0, addr, addr_len);
    gettimeofday(start_time, NULL);

    mem_free(payload);
    return sent;
}

static void rx(int fd, const struct timeval *start_time, const char *server_text,
               uint16_t server_port)
{
    char buf[1024];
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t len;
    struct timeval end_time;
    time_t now;
    struct tm timeinfo;

    while ((len = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen)) > 0)
    {
        if ((size_t)len < SIZEOF_DNS_HDR) {
            continue;
        }
        struct dns_hdr *hdr = (struct dns_hdr *)&buf;
        /* Anything else on this socket is not our answer. send_request() asks with id 1. */
        if (ntohs(hdr->id) != 1) {
            continue;
        }

        // Time Keeping
        gettimeofday(&end_time, NULL);
        uint32_t time_ms = DIG_TIME_DIFF_MS(&end_time, start_time);

        // get time and convert to local time
        time(&now);
        localtime_r(&now, &timeinfo);

        const uint8_t *msg = (const uint8_t *)buf;
        const uint8_t *msg_end = msg + len;

        uint8_t opcode = DNS_HDR_GET_OPCODE(hdr);
        const uint8_t rcode = hdr->flags2 & DNS_FLAG2_ERR_MASK;
        char rcode_text[16];
        const char *rcode_name = dns_rcode_name(rcode);
        if (rcode_name == NULL) {
            snprintf(rcode_text, sizeof(rcode_text), "RCODE%u", rcode);
            rcode_name = rcode_text;
        }

        printf(";; Got answer:\n");
        printf(";; ->>HEADER<<- opcode: %u, status: %s, id: %"PRIu16"\n",
               opcode, rcode_name, ntohs(hdr->id));
        printf(";; flags:");
        if (hdr->flags1 & DNS_FLAG1_RESPONSE) {
            printf(" qr");
        }
        if (hdr->flags1 & DNS_FLAG1_AUTHORATIVE) {
            printf(" aa");
        }
        if (hdr->flags1 & DNS_FLAG1_TRUNC) {
            printf(" tc");
        }
        if (hdr->flags1 & DNS_FLAG1_RD) {
            printf(" rd");
        }
        if (hdr->flags2 & DNS_FLAG2_RA) {
            printf(" ra");
        }
        if ((hdr->flags1 & (DNS_FLAG1_RESPONSE | DNS_FLAG1_AUTHORATIVE | DNS_FLAG1_TRUNC |
                            DNS_FLAG1_RD)) == 0 && (hdr->flags2 & DNS_FLAG2_RA) == 0) {
            printf(" ");
        }
        printf("; QUERY: %"PRIu16", ANSWER: %"PRIu16", AUTHORITY: %"PRIu16", ADDITIONAL: %"PRIu16"\n",
                ntohs(hdr->numquestions),
                ntohs(hdr->numanswers),
                ntohs(hdr->numauthrr),
                ntohs(hdr->numextrarr));
        printf("\n");

        if (hdr->flags1 & DNS_FLAG1_TRUNC) {
            printf(";; Warning: the reply is truncated; this dig has no TCP fallback\n\n");
        }

        const uint8_t *ptr = msg + SIZEOF_DNS_HDR;
        bool ok = true;
        printf(";; QUESTION SECTION:\n");
        for (size_t idx = 0; idx < ntohs(hdr->numquestions); idx++)
        {
            ssize_t qlen = dns_get_label(msg, msg_end, ptr, out, sizeof(out));
            if (qlen < 0 || !dns_in_msg(msg, msg_end, ptr + qlen, 4)) {
                printf(";; (malformed question section)\n");
                ok = false;
                break;
            }
            ptr += qlen;

            uint16_t type = rd_u16(ptr);
            ptr += 2;
            uint16_t class = rd_u16(ptr);
            ptr += 2;
            printf(";%s\t\t%s\t%s\n", out, dns_rr_class(class), dns_rr_type(type));
        }
        printf("\n");

        /* AUTHORITY and ADDITIONAL were parsed by nobody, so an NXDOMAIN printed a header
         * and stopped -- the SOA saying which zone denied it was right there and dropped. */
        ok = ok && print_section("ANSWER", msg, msg_end, &ptr, ntohs(hdr->numanswers));
        ok = ok && print_section("AUTHORITY", msg, msg_end, &ptr, ntohs(hdr->numauthrr));
        (void)print_section("ADDITIONAL", msg, msg_end, &ptr, ok ? ntohs(hdr->numextrarr) : 0);

        printf(";; Query time: %"PRIu32" msec\n", time_ms);
        printf(";; SERVER: %s#%"PRIu16"(%s) (UDP)\n", server_text, server_port, server_text);
        strftime(out, sizeof(out), "%a %b %d %H:%M:%S %Z %Y", &timeinfo);
        printf(";; WHEN: %s\n", out);
        printf(";; MSG SIZE  rcvd: %d\n\n", (int)len);

        /* One question, one answer. Looping until the 1 s receive timeout expired made
         * every lookup cost a second it had no use for. */
        return;
    }

    printf(";; connection timed out; no servers could be reached\n\n");
}

/**
 * @brief Fill a sockaddr from an lwIP address, and report the socket family it needs.
 *
 * @return the sockaddr length, or 0 if this build has no transport for that family.
 */
static socklen_t dig_sockaddr(const ip_addr_t *ipaddr, uint16_t port,
                              struct sockaddr_storage *store, int *family)
{
    memset(store, 0, sizeof(*store));
    *family = AF_UNSPEC;
#if LWIP_IPV6
    if (IP_IS_V6(ipaddr)) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)store;
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(port);
        inet6_addr_from_ip6addr(&sa6->sin6_addr, ip_2_ip6(ipaddr));
        /* A link-local server is only reachable through the interface that learned it,
         * and lwIP will not guess which one. */
        sa6->sin6_scope_id = ip6_addr_has_zone(ip_2_ip6(ipaddr))
                                 ? ip6_addr_zone(ip_2_ip6(ipaddr)) : 0;
        *family = AF_INET6;
        return sizeof(*sa6);
    }
#endif
#if LWIP_IPV4
    struct sockaddr_in *sa4 = (struct sockaddr_in *)store;
    sa4->sin_family = AF_INET;
    sa4->sin_port = htons(port);
    inet_addr_from_ip4addr(&sa4->sin_addr, ip_2_ip4(ipaddr));
    *family = AF_INET;
    return sizeof(*sa4);
#else
    return 0;
#endif
}

static void do_lookup(const ip_addr_t *ipaddr, uint16_t port, const char *hostname,
                      uint16_t q_type, const ip_addr_t *bind_addr, uint16_t bind_port)
{
    struct timeval start_time;
    struct sockaddr_storage to;
    int family = AF_UNSPEC;

    /* ipaddr_ntoa() hands back a shared static buffer, so take a copy before anything
     * else can call it. */
    char server_text[48];
    snprintf(server_text, sizeof(server_text), "%s", ipaddr_ntoa(ipaddr));

    const socklen_t to_len = dig_sockaddr(ipaddr, port, &to, &family);
    if (to_len == 0) {
        printf("dig: no transport for %s\n", server_text);
        return;
    }

    int fd = socket(family, SOCK_DGRAM, IP_PROTO_UDP);
    if (fd < 0) {
        printf("dig: cannot create socket (errno %d)\n", errno);
        return;
    }

    if (bind_addr != NULL) {
        struct sockaddr_storage from;
        int bind_family = AF_UNSPEC;
        const socklen_t from_len = dig_sockaddr(bind_addr, bind_port, &from, &bind_family);
        if (from_len == 0 || bind_family != family) {
            printf("dig: the -b source address is not the same family as %s\n", server_text);
            close(fd);
            return;
        }
        if (bind(fd, (const struct sockaddr *)&from, from_len) != 0) {
            printf("dig: cannot bind to the -b source address (errno %d)\n", errno);
            close(fd);
            return;
        }
    }

    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Send The Request To The Specific Server
    if (send_request(fd, (const struct sockaddr *)&to, to_len, hostname, q_type, &start_time) > 0) {
        rx(fd, &start_time, server_text, port);
    } else {
        printf("dig: cannot send the query (errno %d)\n", errno);
    }

    close(fd);
}

/* Map a type mnemonic ("A", "PTR", ...) back to its RR type number. */
static bool dns_type_from_name(const char *name, uint16_t *out_type)
{
    for (size_t idx = 0; idx < sizeof(rr_types) / sizeof(name_mapping_t); idx++) {
        if (strcasecmp(rr_types[idx].type_name, name) == 0) {
            *out_type = rr_types[idx].type;
            return true;
        }
    }
    return false;
}

/* dig prints trailing-dot FQDNs and dns_build_question() needs the terminating dot to
 * close the last label, so normalise here rather than making every caller remember. */
static void dns_make_fqdn(const char *name, char *out_name, size_t out_size)
{
    size_t len = strnlen(name, out_size - 1);
    memcpy(out_name, name, len);
    if (len == 0 || out_name[len - 1] != '.') {
        if (len < out_size - 1) {
            out_name[len++] = '.';
        }
    }
    out_name[len] = '\0';
}

/* "1.2.3.4" -> "4.3.2.1.in-addr.arpa.", and an IPv6 address to its nibble-reversed
 * ".ip6.arpa." form, for -x. */
static bool dns_make_arpa(const char *addr, char *out_name, size_t out_size)
{
    ip_addr_t ip;
    if (!ipaddr_aton(addr, &ip)) {
        return false;
    }
#if LWIP_IPV6
    if (IP_IS_V6_VAL(ip)) {
        /* 32 nibbles, least significant first, then the suffix. */
        static const char hex[] = "0123456789abcdef";
        const ip6_addr_t *a6 = ip_2_ip6(&ip);
        if (out_size < (32 * 2) + sizeof("ip6.arpa.")) {
            return false;
        }
        char *p = out_name;
        for (int word = 3; word >= 0; word--) {
            const uint32_t v = lwip_ntohl(a6->addr[word]);
            for (int nibble = 0; nibble < 8; nibble++) {
                *p++ = hex[(v >> (nibble * 4)) & 0xf];
                *p++ = '.';
            }
        }
        memcpy(p, "ip6.arpa.", sizeof("ip6.arpa."));
        return true;
    }
#endif
#if LWIP_IPV4
    uint32_t host = lwip_ntohl(ip_2_ip4(&ip)->addr);
    return snprintf(out_name, out_size, "%u.%u.%u.%u.in-addr.arpa.",
                    (unsigned)(host & 0xff), (unsigned)((host >> 8) & 0xff),
                    (unsigned)((host >> 16) & 0xff), (unsigned)((host >> 24) & 0xff)) < (int)out_size;
#else
    return false;
#endif
}

static int do_dig_cmd(int argc, char **argv)
{
    uint16_t port = DNS_SERVER_PORT;
    uint16_t type = DNS_RRTYPE_A;
    int nerrors = arg_parse(argc, argv, (void **)&dig_args);

    if (0 != nerrors) {
        arg_print_errors(stderr, dig_args.end, argv[0]);
        return 1;
    }

    /* -4 / -6 pick the transport, and a server of the wrong family is then not a server at
     * all. Without this the loop below handed an IPv6 resolver straight to an AF_INET
     * socket, which took four bytes out of the middle of it and queried nothing. */
    const bool only_v4 = dig_args.ipv4->count > 0;
    const bool only_v6 = dig_args.ipv6->count > 0;
    if (only_v4 && only_v6) {
        printf("dig: -4 and -6 are mutually exclusive\n");
        return 1;
    }

    /* -b: the source address, optionally with a source port after '#'. */
    ip_addr_t bind_ip;
    const ip_addr_t *bind_ptr = NULL;
    uint16_t bind_port = 0;
    if (dig_args.bind->count > 0) {
        char spec[64];
        snprintf(spec, sizeof(spec), "%s", dig_args.bind->sval[0]);
        char *hash = strchr(spec, '#');
        if (hash != NULL) {
            *hash = '\0';
            char *endp = NULL;
            long p = strtol(hash + 1, &endp, 10);
            if (endp == hash + 1 || *endp != '\0' || p < 0 || p > UINT16_MAX) {
                printf("dig: invalid source port in -b %s\n", dig_args.bind->sval[0]);
                return 1;
            }
            bind_port = (uint16_t)p;
        }
        if (!ipaddr_aton(spec, &bind_ip)) {
            printf("dig: invalid source address %s\n", spec);
            return 1;
        }
        bind_ptr = &bind_ip;
    }

    if (dig_args.port->count > 0) {
        int tmp = dig_args.port->ival[0];
        if (tmp < 0 || tmp > UINT16_MAX) {
            printf("port must be between 0 and %"PRIu16"\n", UINT16_MAX);
            return 1;
        }
        port = (uint16_t)dig_args.port->ival[0];
    }

    // type (the -t option overrides the "type")
    if (dig_args.t_type->count > 0 && dig_args.type->count > 0) {
        printf("cannot specify both -t and type\n");
        return 1;
    }
    const char *type_name = NULL;
    if (dig_args.t_type->count > 0) {
        type_name = dig_args.t_type->sval[0];
    } else if (dig_args.type->count > 0) {
        type_name = dig_args.type->sval[0];
    }
    if (type_name != NULL && !dns_type_from_name(type_name, &type)) {
        printf("unknown query type %s\n", type_name);
        return 1;
    }

    // name (the -q option overrides the "name" )
    char qname[256];
    if (dig_args.reverse->count > 0) {
        // -x: reverse lookup, which implies PTR unless the user asked for something else
        if (!dns_make_arpa(dig_args.reverse->sval[0], qname, sizeof(qname))) {
            printf("invalid address for -x: %s\n", dig_args.reverse->sval[0]);
            return 1;
        }
        if (type_name == NULL) {
            type = DNS_RRTYPE_PTR;
        }
    } else {
        const char *name = NULL;
        if (dig_args.q_name->count > 0) {
            name = dig_args.q_name->sval[0];
        } else if (dig_args.name->count > 0) {
            name = dig_args.name->sval[0];
        }
        if (name == NULL) {
            printf("no name to look up (give a name, -q <name> or -x <addr>)\n");
            return 1;
        }
        dns_make_fqdn(name, qname, sizeof(qname));
    }

    printf("; <<>> dig <<>> %s %s\n", qname, dns_rr_type(type));

    if (dig_args.server->count > 0) {
        // sval[0] still carries the leading '@' the IP_REGEX matched on
        ip_addr_t ip;
        if (ipaddr_aton(&dig_args.server->sval[0][1], &ip) != 1) {
            printf("invalid server address %s\n", dig_args.server->sval[0]);
            return 1;
        }
        if ((only_v4 && IP_IS_V6_VAL(ip)) || (only_v6 && !IP_IS_V6_VAL(ip))) {
            printf("dig: %s is not an IPv%c address\n", &dig_args.server->sval[0][1],
                   only_v6 ? '6' : '4');
            return 1;
        }
        printf("dns server %s\n", ipaddr_ntoa(&ip));
        do_lookup(&ip, port, qname, type, bind_ptr, bind_port);
    } else {
        // Use the local server
        bool queried = false;
        bool skipped = false;
        for (size_t idx = 0; idx < DNS_MAX_SERVERS; idx++) {
            const ip_addr_t *ip = dns_getserver(idx);
            if (ip_addr_cmp(ip, IP_ADDR_ANY)) {
                continue;
            }
            if ((only_v4 && IP_IS_V6(ip)) || (only_v6 && !IP_IS_V6(ip))) {
                skipped = true;
                continue;
            }
            printf("dns server %s\n", ipaddr_ntoa(ip));
            do_lookup(ip, port, qname, type, bind_ptr, bind_port);
            queried = true;
        }
        if (!queried) {
            if (skipped) {
                printf("dig: no IPv%c DNS server configured\n", only_v6 ? '6' : '4');
            } else {
                printf("dig: no DNS server configured\n");
            }
            return 1;
        }
    }

    return 0;
}

void register_dig(void)
{
    dig_args.server = arg_rex0(NULL, NULL, IP_REGEX, "@server", 0, "The server to query");
    dig_args.bind = arg_rex0("b", NULL, BIND_REGEX, "address", 0, "Set the source IP (and optionally #port) of the query");
    dig_args.port = arg_int0("p", NULL, "port", "Set the port on the server to use, defaults to 53");
    dig_args.t_type = arg_rex0("t", NULL, TYPE_REGEX, "type", ARG_REX_ICASE, "the resource type to query");
    dig_args.q_name = arg_str0("q", NULL, "name", "the domain name to query");
    dig_args.reverse = arg_rex0("x", NULL, REVERSE_REGEX, "addr", 0, "simplified reverse lookup");
    dig_args.ipv4 = arg_lit0("4", NULL, "use IPv4 only");
    dig_args.ipv6 = arg_lit0("6", NULL, "use IPv6 only");
    dig_args.name = arg_str0(NULL, NULL, "name",  "the name of the resource record to be queried");
    dig_args.type = arg_rex0(NULL, NULL, TYPE_REGEX, "type", ARG_REX_ICASE, "the type to query, defaults to A if not provided");
    dig_args.end = arg_end(1);

    const esp_console_cmd_t dig_cmd = {
        .command = "dig",
        .help = "DNS lookup utility",
        .hint = NULL,
        .func = &do_dig_cmd,
        .argtable = &dig_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&dig_cmd));
}
