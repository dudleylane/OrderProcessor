#!/bin/sh
# nginx-tls-check.sh: run by the nginx image's entrypoint before nginx starts (#226). The stack serves TLS only, so
# without a certificate and key nginx can read, the container stops here, saying how to make them. nginx's own error
# says only that a file can't be loaded.
set -eu
dir=/etc/nginx/certs
for file in tls.crt tls.key; do
    if [ ! -r "$dir/$file" ]; then
        echo "oms-frontend: can't read $dir/$file, so it can't serve TLS (#226)." \
            "Put a certificate and its key in docker/certs, as tls.crt and tls.key: docker/README.md's Quick Start" \
            "makes a self-signed one. nginx runs as nginx, in group 0, so the key must be readable by group 0 in the" \
            "container: chmod 0640 on the host, and under Docker running as root also chgrp 0." >&2
        exit 1
    fi
done
