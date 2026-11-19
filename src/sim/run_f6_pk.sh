#!/bin/bash
# Ф6: the host-key signature checks (src/fw/pk.c) against Python cryptography vectors (src/tools/pk_vectors.py),
# ASan + UBSan. usage: run_f6_pk.sh [seed 1] [keys per kind 3]
set -e -o pipefail
cd "$(dirname "$0")/../fw"
MC=../../upstream/monocypher/src
mkdir -p build-host
cc -O1 -g -std=c99 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I$MC -I$MC/optional -I. \
   -o build-host/test_pk test_pk.c pk.c sha256.c $MC/monocypher.c $MC/optional/monocypher-ed25519.c
python3 -W ignore ../tools/pk_vectors.py "${1:-1}" "${2:-3}" > build-host/pk_vectors.txt
./build-host/test_pk < build-host/pk_vectors.txt
