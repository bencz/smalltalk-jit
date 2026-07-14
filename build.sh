#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Descobre o número de processadores de forma mais portátil.
if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
elif command -v getconf >/dev/null 2>&1; then
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
else
    JOBS=1
fi

# Proteção básica antes do rm -rf.
case "$BUILD_DIR" in
    ""|"/")
        printf 'Diretório de build inválido: %s\n' "$BUILD_DIR" >&2
        exit 1
        ;;
esac

printf 'Configurando build em: %s\n' "$BUILD_DIR"
printf 'Tipo de build: %s\n' "$BUILD_TYPE"
printf 'Compilação paralela: %s processos\n' "$JOBS"

rm -rf -- "$BUILD_DIR"

cmake \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

cmake \
    --build "$BUILD_DIR" \
    --parallel "$JOBS"

"$SCRIPT_DIR/run_tests.sh" --all
