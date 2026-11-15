#!/usr/bin/env bash
# Builds and (re)starts the test server with fresh mail on 127.0.0.1: IMAPS 9993, IMAP+STARTTLS 9143,
# SMTP submission 9465 (TLS) and 9587 (STARTTLS), ManageSieve 14190, remote test images on http 9080. Writes its CA certificate to $1 (default
# ~/.cache/jackdaw/imap-test/ca.crt): run jackdaw with SSL_CERT_FILE=<that file> to trust it.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CA=${1:-$HOME/.cache/jackdaw/imap-test/ca.crt}
docker build -q -t jackdaw-imap-test "$HERE" >/dev/null
docker rm -f jackdaw-imap-test >/dev/null 2>&1 || true
docker run -d --name jackdaw-imap-test -p 127.0.0.1:9993:993 -p 127.0.0.1:9143:143 -p 127.0.0.1:9465:465 \
    -p 127.0.0.1:9587:587 -p 127.0.0.1:14190:4190 -p 127.0.0.1:9080:80 jackdaw-imap-test >/dev/null
mkdir -p "$(dirname "$CA")"
docker cp jackdaw-imap-test:/etc/dovecot/ca.crt "$CA" >/dev/null
for _ in $(seq 50); do
    (exec 3<>/dev/tcp/127.0.0.1/9993) 2>/dev/null && { echo "IMAP test server up; CA in $CA"; exit 0; }
    sleep 0.2
done
echo "the server did not come up:" >&2
docker logs jackdaw-imap-test >&2
exit 1
