#!/bin/sh
set -eu

fail() {
    printf '%s\n' "boundary audit: $*" >&2
    exit 1
}

if grep -R -n -E '#include[[:space:]]*<(openssl|mbedtls|wolfssl|event2|uv|curl)/' \
    include src; then
    fail "external dependency include found"
fi

if grep -R -n -E '(^|[^[:alnum:]_])(mcp|executor|sandbox|datalog|mir|receipt)([^[:alnum:]_]|$)' \
    include src; then
    fail "domain vocabulary found in the systems layer"
fi

if grep -R -n -E 'pthread_|struct[[:space:]]+(epoll_event|kevent|pollfd)' include; then
    fail "native implementation type leaked through public headers"
fi

if grep -n -E '^Libs:.*-l(event|uv|ssl|crypto|curl)' pkgconfig/maelys-sys.pc.in; then
    fail "external dependency leaked through pkg-config"
fi

if grep -n -E '(^|[^[:alnum:]_])(getaddrinfo|getnameinfo|HTTP|HTTPS|URI|TLS|proxy|SSRF|OCI|MCP)([^[:alnum:]_]|$)' \
    include/maelys/sys/socket.h src/socket.c; then
    fail "name resolution or product policy leaked into the mechanical socket layer"
fi

printf '%s\n' "boundary audit: ok"
