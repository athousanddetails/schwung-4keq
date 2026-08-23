#!/usr/bin/env bash
# Build 4K EQ for Ableton Move (aarch64) — from the Mac.
#
# The Mac has no toolchain and no Docker (deliberately). This script:
#   1. rsyncs the source up to the VPS (~/schwung-dev/schwung-4keq)
#   2. builds there inside Docker (ubuntu:22.04 = glibc 2.35, matching Move)
#   3. rsyncs build/ and dist/ back down
#
#   ./scripts/build.sh [cmake-target]        (default: all)
set -euo pipefail

VPS="vps"
REMOTE_DIR="schwung-dev/schwung-4keq"
TARGET="${1:-all}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== 1/3 rsync source -> $VPS:$REMOTE_DIR"
ssh "$VPS" "mkdir -p $REMOTE_DIR"
rsync -az --delete \
    --exclude .git --exclude build/ --exclude build-native/ \
    --exclude dist/ --exclude .DS_Store \
    "$SRC/" "$VPS:$REMOTE_DIR/"

echo "=== 2/3 docker build on $VPS"
# rsync preserves Mac mtimes and the two clocks differ, so make/ninja can
# decide everything is already up to date and skip the build silently.
# Touching the tree after transfer is what stops a "successful" no-op build.
#
# The docker group may or may not be applied to the login shell, so try a
# plain docker first and only fall back to `sg docker`. Hard-coding `sg`
# fails on a host where the group does not exist at all, and hard-coding the
# plain form fails on one where it is merely unapplied.
ssh "$VPS" "cd $REMOTE_DIR && find src tools tests -type f -exec touch {} + && \
    BUILD_CMD='
    docker image inspect schwung-4keq-builder >/dev/null 2>&1 || \
        docker build -t schwung-4keq-builder -f scripts/Dockerfile scripts/ ;
    docker run --rm -v \$PWD:/build -u \$(id -u):\$(id -g) -w /build \
        schwung-4keq-builder ./scripts/docker-build.sh $TARGET' ; \
    if docker info >/dev/null 2>&1; then \
        sh -c \"\$BUILD_CMD\" ; \
    else \
        echo '(docker group not applied to this shell; retrying under sg)' ; \
        sg docker -c \"\$BUILD_CMD\" ; \
    fi"

echo "=== 3/3 rsync artifacts back"
rsync -az "$VPS:$REMOTE_DIR/build/" "$SRC/build/"
rsync -az "$VPS:$REMOTE_DIR/dist/" "$SRC/dist/" 2>/dev/null || true

echo; echo "=== Artifacts ==="
ls -la "$SRC/build/" | grep -E "\.so|loadtest" || true
