# syntax=docker/dockerfile:1.7

# musl static build. Distroless static has no libc, so -static-libstdc++ on glibc
# does not belong here. linux-headers in this image are new enough for io_uring.
# If a sandbox rejects io_uring_setup, the reactor uses epoll. --epoll forces that.
FROM alpine:3.20 AS builder

ARG TARGETARCH
ARG KESTREL_MARCH=

# Alpine 3.20 has no liburing-static package. liburing-dev ships liburing.a.
# The name ninja is virtual (samurai). The default generator is Unix Makefiles,
# and build-base already provides make.
RUN apk add --no-cache \
    build-base \
    cmake \
    linux-headers \
    liburing-dev \
    file

WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/

RUN set -eux; \
    if [ -n "${KESTREL_MARCH}" ]; then \
      march="${KESTREL_MARCH}"; \
    elif [ "${TARGETARCH}" = "amd64" ]; then \
      march="x86-64-v3"; \
    elif [ "${TARGETARCH}" = "arm64" ]; then \
      march="armv8-a+simd"; \
    else \
      march="native"; \
    fi; \
    cmake -S . -B build/release \
      -DCMAKE_BUILD_TYPE=Release \
      -DKESTREL_STATIC=ON \
      -DKESTREL_MARCH="${march}"; \
    cmake --build build/release -j"$(nproc)"; \
    ctest --test-dir build/release --output-on-failure; \
    file build/release/kestreld | grep -Eq 'statically linked|static-pie linked'

FROM gcr.io/distroless/static-debian12:nonroot

LABEL org.opencontainers.image.title="kestreld" \
      org.opencontainers.image.description="Shared-nothing C++20 cache and inference gateway. Identical prompts hit an on-core index." \
      org.opencontainers.image.source="https://github.com/Arlikhozhaev/kestrel"

COPY --from=builder --chown=65532:65532 /src/build/release/kestreld /usr/local/bin/kestreld

USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/kestreld"]
CMD ["--bind", "0.0.0.0", "--port", "8080", "--wal", "/home/nonroot/wal"]
