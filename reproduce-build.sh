#!/bin/sh
# Rebuilds game-library-manager.elf from source/ and checks it matches the
# published glm-0.3.9.elf byte for byte. Needs Docker. Run from the repo root:
#   sh reproduce-build.sh
set -e
docker run --rm -v "$(pwd):/repo" ubuntu:24.04 sh -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq clang-18 lld-18 llvm-18 make python3 curl unzip ca-certificates >/dev/null
  curl -sSL -o /tmp/sdk.zip https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
  unzip -q /tmp/sdk.zip -d /opt
  cp -r /repo/source /tmp/glm && cd /tmp/glm
  PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk make >/dev/null
  sha256sum game-library-manager.elf /repo/glm-0.3.9.elf
  cmp game-library-manager.elf /repo/glm-0.3.9.elf && echo "MATCH: glm-0.3.9.elf was built from this source"
'
