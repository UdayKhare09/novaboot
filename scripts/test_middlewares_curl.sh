#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-https://localhost:4433}"
CURL=(curl --http3-only -k -sS --max-time 10)

pass() { printf 'PASS %s\n' "$1"; }
fail() {
    printf 'FAIL %s\n' "$1" >&2
    exit 1
}

status_of() {
    "${CURL[@]}" -o /tmp/novaboot-curl-body.txt -w '%{http_code}' "$@"
}

header_of() {
    local name="$1"
    local file="$2"
    awk -v n="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')" '
        BEGIN { FS=":" }
        tolower($1) == n {
            sub(/^[ \t]+/, "", $2)
            sub(/\r$/, "", $2)
            print $2
            exit
        }
    ' "$file"
}

TOKEN="$(python - <<'PY'
import base64, hashlib, hmac, json, time

secret = b"sample-secret"
now = int(time.time())
header = {"alg": "HS256", "typ": "JWT"}
payload = {
    "sub": "curl-user",
    "iss": "novaboot-sample",
    "aud": "sample-api",
    "scope": "read write",
    "iat": now - 10,
    "exp": now + 3600,
}

def enc(obj):
    raw = json.dumps(obj, separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(raw).rstrip(b"=")

signing = enc(header) + b"." + enc(payload)
sig = base64.urlsafe_b64encode(
    hmac.new(secret, signing, hashlib.sha256).digest()
).rstrip(b"=")
print((signing + b"." + sig).decode())
PY
)"

headers="$(mktemp)"
body="$(mktemp)"
trap 'rm -f "$headers" "$body" /tmp/novaboot-curl-body.txt' EXIT

"${CURL[@]}" -D "$headers" -o "$body" "$BASE_URL/" >/dev/null
[[ "$(head -n 1 "$headers")" == *" 200 "* ]] || fail "public root returns 200"
[[ -n "$(header_of strict-transport-security "$headers")" ]] || fail "security header present"
pass "public root and security headers"

"${CURL[@]}" -D "$headers" -o "$body" -H 'Accept-Encoding: gzip' "$BASE_URL/" >/dev/null
[[ "$(header_of content-encoding "$headers")" == "gzip" ]] || fail "gzip compression enabled"
pass "gzip compression"

code="$(status_of "$BASE_URL/api/users")"
[[ "$code" == "401" ]] || fail "private route without token returned $code, expected 401"
pass "private route rejects missing JWT"

code="$(status_of -H "Authorization: Bearer $TOKEN" "$BASE_URL/api/users")"
[[ "$code" == "200" ]] || fail "private route with JWT returned $code, expected 200"
pass "private route accepts valid JWT"

large_body="$(mktemp)"
python - <<'PY' > "$large_body"
print("x" * (64 * 1024 + 1))
PY
code="$(status_of -X POST -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' --data-binary "@$large_body" \
    "$BASE_URL/api/users")"
rm -f "$large_body"
[[ "$code" == "413" ]] || fail "large body returned $code, expected 413"
pass "body size limit"

printf 'All middleware curl checks passed against %s\n' "$BASE_URL"
