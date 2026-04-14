#!/usr/bin/env bash
# Build gabcode. Run tests with `./build.sh test`. Wipe build/ with `./build.sh clean`.
set -e

cd "$(dirname "$0")"

case "${1:-}" in
    clean)
        rm -rf build gabcode
        echo "Cleaned build/ and gabcode binary."
        exit 0
        ;;
    test)
        cmake -B build -DGABCODE_BUILD_TESTS=ON >/dev/null
        cmake --build build -j
        ctest --test-dir build --output-on-failure
        exit 0
        ;;
esac

cmake -B build >/dev/null
cmake --build build -j

echo ""
echo "Built ./gabcode. Run it with:"
echo "  ./gabcode"
