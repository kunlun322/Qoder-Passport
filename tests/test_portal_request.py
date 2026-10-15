"""Offline regression of the real C portal guard with synthetic HTTP/socket stubs.

Default: compile and execute with CC (or cc); no device or real network access.
--contracts-only: source contracts, not C execution.
--syntax-only --cc <cross-compiler>: compile checks only, not C execution.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/app_portal.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", SOURCE)
    if not match:
        raise AssertionError(f"Missing production function: {name}")
    start = SOURCE.rfind("\n", 0, match.start()) + 1
    depth = 1
    tokens = re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\{|\}',
                         SOURCE[match.end():])
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:match.end() + token.end()]
    raise AssertionError(f"Unclosed production function: {name}")


class Contracts(unittest.TestCase):
    def test_full_socket_storage(self):
        body = function("ap_request")
        self.assertIn("struct sockaddr_storage local = {0}", body)
        self.assertIn("socklen_t len = sizeof(local)", body)
        self.assertIn("ap_local_address(&local, len)", body)

    def test_family_and_size_before_copy(self):
        body = function("ap_local_address")
        for family, structure in [("AF_INET", "sockaddr_in"), ("AF_INET6", "sockaddr_in6")]:
            self.assertIn(f"local->ss_family == {family}", body)
            self.assertIn(f"len != sizeof(struct {structure})", body)
        self.assertIn("#if CONFIG_LWIP_IPV6", body)
        self.assertIn("return false", body)

    def test_all_sensitive_handlers_still_guarded(self):
        for name in ("root_get", "status_get", "scan_get", "save_post"):
            self.assertRegex(function(name), r"\{\s*if \(!ap_request\(req\)\)")

    def test_host_get_body_and_csrf_guards_retained(self):
        body = function("ap_request")
        self.assertIn("req->method == HTTP_GET && req->content_len != 0", body)
        self.assertIn('strcmp(host, "192.168.4.1")', body)
        self.assertIn('strcmp(host, "192.168.4.1:80")', body)
        self.assertIn('"X-Portal-Token"', function("save_post"))
        self.assertIn("strcmp(token, s_token)", function("save_post"))


# lwIP-compatible ABI fixtures (including its one-byte length/family fields).
# The two production functions below are extracted unchanged, never reimplemented.
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define AF_INET 2
#define AF_INET6 10
#define HTTP_GET 0
#define HTTP_POST 1
#define ESP_OK 0
typedef uint32_t socklen_t;
struct in_addr { uint32_t s_addr; };
struct in6_addr { uint8_t s6_addr[16]; };
struct sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct sockaddr_in {
    uint8_t sin_len, sin_family; uint16_t sin_port;
    struct in_addr sin_addr; char sin_zero[8];
};
struct sockaddr_in6 {
    uint8_t sin6_len, sin6_family; uint16_t sin6_port;
    uint32_t sin6_flowinfo; struct in6_addr sin6_addr; uint32_t sin6_scope_id;
};
struct sockaddr_storage {
    uint8_t s2_len, ss_family; char s2_data1[2]; uint32_t s2_data2[3];
#if CONFIG_LWIP_IPV6
    uint32_t s2_data3[3];
#endif
};
typedef struct { int method; size_t content_len; const char *host; } httpd_req_t;
static struct sockaddr_storage socket_address;
static socklen_t socket_length, requested_length;
static int socket_error, socket_calls;
static uint32_t inet_addr(const char *text) {
    unsigned a, b, c, d; uint32_t out;
    assert(sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) == 4);
    const uint8_t bytes[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    memcpy(&out, bytes, sizeof(out)); return out;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *key,
                                      char *out, size_t cap) {
    assert(strcmp(key, "Host") == 0);
    if (!r->host || strlen(r->host) >= cap) return -1;
    strcpy(out, r->host); return ESP_OK;
}
static int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return 7; }
static int getsockname(int fd, struct sockaddr *out, socklen_t *len) {
    assert(fd == 7); socket_calls++; requested_length = *len;
    if (socket_error) return -1;
    const size_t copied = *len < socket_length ? *len : socket_length;
    assert(copied <= sizeof(socket_address));
    memcpy(out, &socket_address, copied);
    // lwIP truncates to caller capacity, unlike platforms that return full size.
    if (*len > socket_length) *len = socket_length;
    return 0;
}
'''

CASES = r'''
static void ipv4(uint32_t ip) {
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = ip;
    memset(&socket_address, 0, sizeof(socket_address));
    memcpy(&socket_address, &a, sizeof(a)); socket_length = sizeof(a);
    socket_error = 0;
}
int main(void) {
    const uint32_t ap = inet_addr("192.168.4.1");
    httpd_req_t req = {HTTP_GET, 0, "192.168.4.1"};
    assert(sizeof(struct sockaddr_in) == 16);
    ipv4(ap); assert(ap_request(&req));
    assert(requested_length == sizeof(struct sockaddr_storage));
    req.host = "192.168.4.1:80"; assert(ap_request(&req));
    req.method = HTTP_POST; req.content_len = 8; assert(ap_request(&req));
    req.method = HTTP_GET; req.content_len = 1; assert(!ap_request(&req));
    req.content_len = 0;
    const char *bad_hosts[] = {NULL, "example.test", "192.168.4.1:81", "192.168.4.10",
        "192.168.4.1.evil.test", "192.168.4.1@evil.test", "192.168.4.1 ",
        "192.168.4.1\r\nX:1", "0123456789012345678901234567890123456789"};
    for (size_t i = 0; i < sizeof(bad_hosts)/sizeof(bad_hosts[0]); i++) {
        req.host = bad_hosts[i]; const int before = socket_calls;
        assert(!ap_request(&req)); assert(socket_calls == before);
    }
    req.host = "192.168.4.1";
    socket_error = 1; assert(!ap_request(&req)); socket_error = 0;
    const char *bad_ips[] = {"0.0.0.0", "127.0.0.1", "192.168.4.2", "192.168.1.1"};
    for (size_t i = 0; i < sizeof(bad_ips)/sizeof(bad_ips[0]); i++) {
        ipv4(inet_addr(bad_ips[i])); assert(!ap_request(&req));
    }
    ipv4(ap);
    for (socklen_t n = 0; n < sizeof(struct sockaddr_in); n++) {
        socket_length = n; assert(!ap_request(&req));
    }
    ipv4(ap); socket_address.ss_family = 99; assert(!ap_request(&req));
    assert(!ap_local_address(NULL, 0));
    ipv4(ap); assert(!ap_local_address(&socket_address, sizeof(socket_address) + 1));
#if CONFIG_LWIP_IPV6
    assert(sizeof(struct sockaddr_in6) == 28);
    struct sockaddr_in6 a = {0}; a.sin6_family = AF_INET6;
    const uint8_t mapped[16] = {0,0,0,0,0,0,0,0,0,0,255,255,192,168,4,1};
    memcpy(a.sin6_addr.s6_addr, mapped, sizeof(mapped));
    memset(&socket_address, 0, sizeof(socket_address));
    memcpy(&socket_address, &a, sizeof(a)); socket_length = sizeof(a);
    assert(ap_request(&req)); assert(requested_length == sizeof(a));
    assert(!ap_local_address(&socket_address, sizeof(a) + 1));
    req.content_len = 1; assert(!ap_request(&req));
    req.method = HTTP_POST; req.content_len = 8; assert(ap_request(&req));
    req.method = HTTP_GET; req.content_len = 0;
    // All single-byte address changes, including mapped-prefix changes, must fail.
    for (size_t i = 0; i < sizeof(mapped); i++) {
        a.sin6_addr.s6_addr[i] ^= 1; memcpy(&socket_address, &a, sizeof(a));
        assert(!ap_request(&req)); a.sin6_addr.s6_addr[i] ^= 1;
    }
    memcpy(&socket_address, &a, sizeof(a));
    for (socklen_t n = 0; n < sizeof(a); n++) {
        socket_length = n; assert(!ap_request(&req));
    }
    socket_length = sizeof(a);
    // Native IPv6 and IPv4-compatible IPv6 are not AP access, even with spoofed flowinfo.
    memset(a.sin6_addr.s6_addr, 0, 16); a.sin6_flowinfo = ap;
    memcpy(&socket_address, &a, sizeof(a)); assert(!ap_request(&req));
    a.sin6_addr.s6_addr[15] = 1; memcpy(&socket_address, &a, sizeof(a));
    assert(!ap_request(&req));
    a.sin6_addr.s6_addr[0] = 0xfe; a.sin6_addr.s6_addr[1] = 0x80;
    memcpy(&socket_address, &a, sizeof(a)); assert(!ap_request(&req));
    memset(a.sin6_addr.s6_addr, 0, 16); memcpy(a.sin6_addr.s6_addr + 12, mapped + 12, 4);
    memcpy(&socket_address, &a, sizeof(a)); assert(!ap_request(&req));
#else
    socket_address.ss_family = AF_INET6; assert(!ap_request(&req));
#endif
    puts("Portal guard C regression: PASS (synthetic socket/HTTP only)");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contracts-only", action="store_true")
    parser.add_argument("--syntax-only", action="store_true")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Contracts)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    if args.contracts_only:
        print("Source contracts: PASS; C execution: NOT RUN")
        return
    # No cwd mutation, device access, downloads, or repository-generated files.
    with tempfile.TemporaryDirectory(prefix="ai-passport-portal-test-") as temp:
        source = Path(temp) / "guard.c"
        executable = Path(temp) / ("guard.exe" if os.name == "nt" else "guard")
        source.write_text(PRELUDE + function("ap_local_address") + "\n" +
                          function("ap_request") + CASES, encoding="utf-8")
        # A Windows executable path can contain spaces/backslashes; do not shell-split it.
        compiler = [args.cc] if Path(args.cc).is_file() else shlex.split(args.cc)
        for ipv6 in (0, 1):
            cmd = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                              f"-DCONFIG_LWIP_IPV6={ipv6}"]
            if args.syntax_only:
                cmd += ["-fsyntax-only", str(source)]
            else:
                cmd += [str(source), "-o", str(executable)]
            subprocess.run(cmd, check=True)
            if not args.syntax_only:
                subprocess.run([str(executable)], check=True)
        if args.syntax_only:
            print("C syntax: PASS (IPv4-only + dual-stack); C execution: NOT RUN")


if __name__ == "__main__":
    main()
