#!/bin/sh
# nginx-public-host-check.sh: run by the nginx image's entrypoint before nginx starts (#564). OMS_PUBLIC_HOST is where
# plain HTTP is redirected and the socket the CSP lets the app open, so it is a host and an optional port, such as
# oms.example.com:3000. A scheme or a path written into it, as OMS_ALLOWED_ORIGINS beside it takes, would redirect to
# https://https://... and name no socket, so the container stops here, saying what it takes.
set -eu
if ! printf '%s' "${OMS_PUBLIC_HOST:-}" | grep -Eqx '(([A-Za-z0-9-]+\.)*[A-Za-z0-9-]+|\[[0-9A-Fa-f:.]+\])(:[0-9]{1,5})?'; then
    echo "oms-frontend: OMS_PUBLIC_HOST is '${OMS_PUBLIC_HOST:-}', not a host and an optional port, such as" \
        "oms.example.com:3000 or localhost:3000, with no scheme and no path (#564)" >&2
    exit 1
fi
