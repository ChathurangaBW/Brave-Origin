#!/bin/bash
# build-linux.sh — build the Linux CLI (run inside WSL, e.g. kali-linux).
#   bash build-linux.sh
# Requires: g++ with C++17 support. Output: dist/brave-origin-fix-linux
set -e
cd "$(dirname "$0")"
mkdir -p dist build
echo "Compiler provenance:"
g++ --version | head -2
echo "COMMAND: g++ -std=c++17 -O2 -Wall -Wextra -Isrc src/platform_linux.cpp src/cli_linux.cpp -o dist/brave-origin-fix-linux"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc src/platform_linux.cpp src/cli_linux.cpp -o dist/brave-origin-fix-linux
echo "Build OK: dist/brave-origin-fix-linux"
ls -l dist/brave-origin-fix-linux
