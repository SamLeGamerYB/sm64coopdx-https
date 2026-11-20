#!/usr/bin/env bash
set -euo pipefail

CURL_VERSION="${CURL_VERSION:-8.23.0}"
: "${CURL_PREFIX:?Set CURL_PREFIX to the install prefix for the static libcurl build}"
: "${OPENSSL_PREFIX:?Set OPENSSL_PREFIX to the OpenSSL installation prefix}"

WORK_DIR="${CURL_BUILD_DIR:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sm64coopdx-curl-build}"
ARCHIVE="$WORK_DIR/curl-$CURL_VERSION.tar.xz"
SOURCE_DIR="$WORK_DIR/curl-$CURL_VERSION"

mkdir -p "$WORK_DIR" "$CURL_PREFIX"
curl --fail --location --retry 3 "https://curl.se/download/curl-$CURL_VERSION.tar.xz" --output "$ARCHIVE"
tar -xJf "$ARCHIVE" -C "$WORK_DIR"

export CPPFLAGS="-I$OPENSSL_PREFIX/include ${CPPFLAGS:-}"
export LDFLAGS="-L$OPENSSL_PREFIX/lib ${LDFLAGS:-}"
export PKG_CONFIG_PATH="$OPENSSL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

CONFIGURE_ARGS=(
  "--prefix=$CURL_PREFIX"
  "--disable-shared"
  "--enable-static"
  "--enable-websockets"
  "--with-openssl=$OPENSSL_PREFIX"
  "--without-zlib"
  "--without-brotli"
  "--without-zstd"
  "--without-libpsl"
  "--without-libssh2"
  "--without-nghttp2"
  "--without-nghttp3"
  "--without-ngtcp2"
  "--without-libidn2"
  "--without-libgsasl"
  "--disable-ldap"
  "--disable-ldaps"
)

if [[ "$(uname -s)" == "Darwin" ]]; then
  CONFIGURE_ARGS+=("--with-apple-sectrust" "--with-ca-fallback")
fi

cd "$SOURCE_DIR"
./configure "${CONFIGURE_ARGS[@]}"
make -j"$(getconf _NPROCESSORS_ONLN)"
make install

if ! "$CURL_PREFIX/bin/curl-config" --protocols | tr ' ' '\n' | grep -qx 'WSS'; then
  echo "The built libcurl does not advertise WSS support" >&2
  exit 1
fi

OPENSSL_LIBDIR="$(dirname "$(find "$OPENSSL_PREFIX" -name libssl.a -print -quit)")"
if [[ -z "$OPENSSL_LIBDIR" || ! -f "$OPENSSL_LIBDIR/libcrypto.a" ]]; then
  echo "Static OpenSSL libraries were not found under $OPENSSL_PREFIX" >&2
  exit 1
fi

if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    echo "CURL_PREFIX=$CURL_PREFIX"
    echo "OPENSSL_LIBDIR=$OPENSSL_LIBDIR"
  } >> "$GITHUB_ENV"
else
  echo "Export CURL_PREFIX=$CURL_PREFIX"
  echo "Export OPENSSL_LIBDIR=$OPENSSL_LIBDIR"
fi
