ARG ALPINE_VERSION=3.24
ARG CURL_VERSION=8.23.0

FROM alpine:${ALPINE_VERSION} AS build

ARG CURL_VERSION

RUN apk add --no-cache \
    build-base \
    cmake \
    meson \
    ninja \
    pkgconf \
    perl \
    ca-certificates \
    openssl-dev \
    openssl-libs-static \
    zlib-dev \
    zlib-static

# libcurl (static)
RUN set -eux; \
    cd /tmp; \
    wget -q "https://curl.se/download/curl-${CURL_VERSION}.tar.xz"; \
    tar -xf "curl-${CURL_VERSION}.tar.xz"; \
    cd "curl-${CURL_VERSION}"; \
    ./configure \
    --prefix=/usr/local \
    --disable-shared \
    --enable-static \
    --with-openssl \
    --with-zlib \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
    --without-libpsl \
    --without-nghttp2 \
    --without-brotli \
    --without-zstd \
    --without-libidn2 \
    --without-librtmp \
    --without-libssh2 \
    --disable-ldap \
    --disable-ldaps; \
    make -j "$(nproc)"; \
    make install; \
    rm -rf /tmp/curl-*

WORKDIR /src
COPY . .

# Static build (uses libcurl from /usr/local above)
RUN set -eux; \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
    meson setup build -Dstatic_build=true --buildtype=release; \
    ninja -C build; \
    strip build/nc2p; \
    ./build/nc2p 2>&1 | head -1 || true

FROM scratch

COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
COPY --from=build /src/build/nc2p /nc2p

EXPOSE 9999
ENTRYPOINT ["/nc2p"]
