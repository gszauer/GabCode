#!/usr/bin/env bash
# Build gabcode CLI and stage the web shell. Run tests with `./build.sh test`. Wipe with `./build.sh clean`.
set -e

cd "$(dirname "$0")"

case "${1:-}" in
    clean)
        rm -rf build web_build gabcode
        echo "Cleaned build/, web_build/, and gabcode binary."
        exit 0
        ;;
    test)
        cmake -B build -DGABCODE_BUILD_TESTS=ON >/dev/null
        cmake --build build -j
        ctest --test-dir build --output-on-failure
        exit 0
        ;;
    web)
        rm -rf web_build
        mkdir -p web_build
        cp -R web/. web_build/
        echo "Staged web/ → web_build/."
        exit 0
        ;;
esac

cmake -B build >/dev/null
cmake --build build -j

rm -rf web_build
mkdir -p web_build
cp -R web/. web_build/

echo ""
echo "Built ./gabcode and staged web/ → web_build/."
echo "Run the CLI with:"
echo "  ./gabcode"
