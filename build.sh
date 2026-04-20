#!/usr/bin/env bash
# Build gab CLI and stage the web shell. Run tests with `./build.sh test`. Wipe with `./build.sh clean`.
set -e

cd "$(dirname "$0")"

# Scan web_build/ for directories containing a SKILL.md and emit
# web_build/bundled-skills.json listing each skill's .md files. The web app
# fetches this manifest at runtime and seeds the files into every chat's
# .gab/skills/<name>/ folder. Drop a new skill folder into web/ and it's
# picked up automatically on the next build — no code changes required.
gen_bundled_skills_manifest() {
    python3 - "$1" <<'PYEOF'
import os, json, sys
base = sys.argv[1]
manifest = {}
for name in sorted(os.listdir(base)):
    path = os.path.join(base, name)
    if not os.path.isdir(path): continue
    if not os.path.isfile(os.path.join(path, "SKILL.md")): continue
    files = []
    for root, _, filenames in os.walk(path):
        for f in filenames:
            if f.endswith(".md"):
                rel = os.path.relpath(os.path.join(root, f), path).replace(os.sep, "/")
                files.append(rel)
    manifest[name] = sorted(files)
with open(os.path.join(base, "bundled-skills.json"), "w") as f:
    json.dump(manifest, f, indent=2)
if manifest:
    print(f"  bundled skills: {', '.join(sorted(manifest.keys()))}")
PYEOF
}

case "${1:-}" in
    clean)
        rm -rf build web_build gab
        echo "Cleaned build/, web_build/, and gab binary."
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
        gen_bundled_skills_manifest web_build
        echo "Staged web/ → web_build/."
        exit 0
        ;;
esac

cmake -B build >/dev/null
cmake --build build -j

rm -rf web_build
mkdir -p web_build
cp -R web/. web_build/
gen_bundled_skills_manifest web_build

echo ""
echo "Built ./gab and staged web/ → web_build/."
echo "Run the CLI with:"
echo "  ./gab"
