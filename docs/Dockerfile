# Local-dev variant of logos-co/logos-docker's Dockerfile.
#
# Difference: builds `delivery_module` from source instead of installing the
# `build-20260422-...` release via lgpd+lgpm. That release was compiled
# against an older logos-cpp-sdk than current `logoscore` HEAD, so RPC replies
# of type `LogosResult` arrive at the daemon as an unknown metatype and get
# serialized as JSON null. Building both from HEAD pins the same
# logos-cpp-sdk revision (verified via flake.lock in both repos), so the
# `LogosResult` ABI matches.

# Stage 1: Build
FROM nixos/nix:2.34.1 AS builder
RUN echo "experimental-features = nix-command flakes" >> /etc/nix/nix.conf
WORKDIR /app

RUN nix build 'github:logos-co/logos-logoscore-cli#cli-appimage' --out-link ./logoscore --refresh
RUN nix build 'github:logos-co/logos-delivery-module#install-portable' --out-link ./delivery --refresh

RUN mkdir -p /app-final/logos /app-final/modules \
    && cp -rL ./logoscore/* /app-final/logos/ \
    && cp -rL ./delivery/modules/delivery_module /app-final/modules/delivery_module

# Stage 2: Runtime
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl netcat-openbsd && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app-final/logos /app/logos
COPY --from=builder /app-final/modules /home/ubuntu/modules

RUN cd /app/logos && chmod a+rx logoscore.AppImage \
    && ./logoscore.AppImage --appimage-extract > /dev/null \
    && mv squashfs-root logoscore \
    && rm logoscore.AppImage \
    && ln -s /app/logos/logoscore/AppRun /bin/logoscore

RUN chown -R ubuntu:ubuntu /home/ubuntu

USER ubuntu
WORKDIR /home/ubuntu

CMD ["logoscore", "-D", "-m", "/home/ubuntu/modules"]
