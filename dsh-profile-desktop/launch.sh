#!/usr/bin/env bash
# dsh desktop launcher: pin the nvm node, pick a free port, boot the
# desktop profile. Installed as the Exec target of dsh-desktop.desktop.
set -u

NVM_BIN="/home/moyamryia/.nvm/versions/node/v24.19.0/bin"
if [ -x "$NVM_BIN/node" ]; then
  export PATH="$NVM_BIN:$PATH"
fi

DSH="$NVM_BIN/dsh"
if [ ! -x "$DSH" ]; then
  DSH="$(command -v dsh || true)"
fi
if [ -z "$DSH" ]; then
  echo "dsh-desktop: dsh not found" >&2
  exit 1
fi

port_in_use() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3>&- 3<&-; return 0; }
  return 1
}

PORT="${DSH_DESKTOP_PORT:-3080}"
while port_in_use "$PORT"; do
  PORT=$((PORT + 1))
  if [ "$PORT" -gt 3099 ]; then
    echo "dsh-desktop: no free port in 3080-3099" >&2
    exit 1
  fi
done

exec "$DSH" --profile desktop --port "$PORT"
