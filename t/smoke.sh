#!/bin/sh

# Smoke test for ngx_http_reqstat_module.
#
# usage: t/smoke.sh /path/to/nginx [/path/to/ngx_http_reqstat_module.so]
#
# Requires curl (with HTTP/2 support), openssl, and a python with
# prometheus_client installed ($PYTHON, default python3).

set -eu

NGINX=$1
MODULE=${2:-}
PYTHON=${PYTHON:-python3}
T=$(dirname "$0")

P1=18181    # http/1.1
P2=18182    # http/2 over tls
P3=18183    # closed port, for upstream errors
P4=18184    # upstream that accepts but never responds

WORK=$(mktemp -d)
mkdir -p "$WORK/logs" "$WORK/conf" "$WORK/html"
echo hello > "$WORK/html/index.html"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
    -keyout "$WORK/conf/key.pem" -out "$WORK/conf/cert.pem" 2>/dev/null

{
    if [ -n "$MODULE" ]; then
        echo "load_module $MODULE;"
    fi

    cat <<EOF
worker_processes 2;
error_log logs/error.log info;
pid logs/nginx.pid;
events { worker_connections 64; }

http {
    access_log logs/access.log;

    req_status_zone srv "p\$server_port" 1M;

    server {
        listen $P1;
        listen $P2 ssl http2;
        ssl_certificate cert.pem;
        ssl_certificate_key key.pem;
        root html;

        req_status srv;

        location /bad { proxy_pass http://127.0.0.1:$P3; }
        location /slow { proxy_pass http://127.0.0.1:$P4; }
        location /teapot { return 418; }
        location /all { req_status_show; req_status_bypass 1; }
        location /sel {
            req_status_show srv;
            req_status_show_field conn_total req_total http_2xx http_404
                                  http_502 ups_req;
            req_status_bypass 1;
        }
        location /codes {
            req_status_show srv;
            req_status_show_field req_total status_codes;
            req_status_bypass 1;
        }
        location /json {
            req_status_show srv;
            req_status_show_format json;
            req_status_bypass 1;
        }
        location /json_key {
            req_status_show srv;
            req_status_show_format json;
            req_status_show_key \$arg_k;
            req_status_bypass 1;
        }
        location /metrics {
            req_status_show;
            req_status_show_format prometheus;
            req_status_bypass 1;
        }
    }
}
EOF
} > "$WORK/conf/nginx.conf"

cleanup() {
    "$NGINX" -p "$WORK" -s stop 2>/dev/null || true
    if [ -n "${HANG_PID:-}" ]; then kill "$HANG_PID" 2>/dev/null || true; fi
    if [ "${KEEP:-}" = "" ]; then rm -rf "$WORK"; fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    echo "--- error.log"
    cat "$WORK/logs/error.log" || true
    exit 1
}

"$NGINX" -p "$WORK" -t
"$NGINX" -p "$WORK"
sleep 1

# HTTP/1.1: one keepalive connection, 2 x 200 + 1 x 404, then one 502

curl -s -o /dev/null -o /dev/null -o /dev/null \
    http://127.0.0.1:$P1/ http://127.0.0.1:$P1/ http://127.0.0.1:$P1/nope
curl -s -o /dev/null http://127.0.0.1:$P1/bad

# exact status codes: 418 has no column of its own; the client gives up
# on a hanging upstream, which nginx logs as 499 with upstream status "-"

curl -s -o /dev/null http://127.0.0.1:$P1/teapot

"$PYTHON" -c '
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(8)
c = s.accept()
time.sleep(60)
' $P4 &
HANG_PID=$!
sleep 0.5

curl -s -o /dev/null --max-time 1 http://127.0.0.1:$P1/slow || true
sleep 0.5

# HTTP/2: one connection, 3 requests

curl -sk --http2 -o /dev/null -o /dev/null -o /dev/null \
    -w '%{http_version}\n' \
    https://localhost:$P2/ https://localhost:$P2/ https://localhost:$P2/ \
    | grep -qx 2 || fail "curl did not negotiate HTTP/2"

check() {
    got=$(curl -s http://127.0.0.1:$P1/sel | grep "^$1," || true)
    [ "$got" = "$2" ] || fail "selected fields: expected '$2', got '$got'"
    echo "ok: $2"
}

#            kv     conn req 2xx 404 502 ups_req
check p$P1 "p$P1,4,6,2,1,1,2"
check p$P2 "p$P2,1,3,3,0,0,0"

# exact status codes, in the order they were first seen

got=$(curl -s http://127.0.0.1:$P1/codes | grep "^p$P1," || true)
want="p$P1,6,200:2;404:1;502:1;418:1;499_upstream:1"
[ "$got" = "$want" ] || fail "status codes: expected '$want', got '$got'"
echo "ok: $want"

# json, same shape as the former lua endpoints

"$PYTHON" - "$P1" "$P2" <<'PYEOF2' || fail "json check"
import json, sys, urllib.request, urllib.error

p1, p2 = sys.argv[1], sys.argv[2]
base = "http://127.0.0.1:%s" % p1

def get(path):
    try:
        r = urllib.request.urlopen(base + path)
        return r.status, r.headers["Content-Type"], json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, e.headers["Content-Type"], json.load(e)

codes1 = {"200": 2, "404": 1, "502": 1, "418": 1, "499_upstream": 1}

status, ctype, body = get("/json")
assert (status, ctype) == (200, "application/json"), (status, ctype)
assert body == {"p" + p1: codes1, "p" + p2: {"200": 3}}, body

status, _, body = get("/json_key?k=p" + p1)
assert status == 200 and body == codes1, (status, body)

status, _, body = get("/json_key?k=nope")
assert status == 404, status
assert body == {"message": "Invalid key", "key": "nope"}, body

print("ok: json")
PYEOF2

# default text format: kv + 29 built-in fields

ncols=$(curl -s http://127.0.0.1:$P1/all | grep "^p$P1," | awk -F, '{print NF}')
[ "$ncols" = 30 ] || fail "expected 30 columns, got $ncols"
echo "ok: 30 columns"

# prometheus format

curl -s -D "$WORK/headers" -o "$WORK/metrics" http://127.0.0.1:$P1/metrics
grep -qi '^content-type: text/plain; version=0.0.4' "$WORK/headers" \
    || fail "bad prometheus content type"

"$PYTHON" "$T/check_prometheus.py" "$WORK/metrics" "p$P1" \
    || fail "prometheus check"

if grep -E '\[(alert|crit|emerg)\]|exited on signal' "$WORK/logs/error.log"
then
    fail "errors in error.log"
fi

echo "PASS"
