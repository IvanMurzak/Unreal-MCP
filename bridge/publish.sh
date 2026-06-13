#!/usr/bin/env bash
# Publish self-contained single-file unreal-mcp-bridge binaries per RID and zip them
# (docs/ARCHITECTURE.md §6 BUNDLE model, issue #45: unreal-mcp-bridge-<rid>.zip release artifacts).
#
# The published binary is SELF-CONTAINED — the end user needs no .NET SDK/runtime installed.
# Trimming is OFF (McpPlugin/ReflectorNet/SignalR are reflection-heavy; see the bridge csproj).
#
# Usage:
#   ./publish.sh [Configuration] [rid ...]
#   ./publish.sh                          # Release, all 4 RIDs, zipped
#   ./publish.sh Release win-x64          # Release, only win-x64, zipped
#   ./publish.sh Release osx-x64 osx-arm64 --no-zip   # publish dirs only (e.g. before signing)
#
# Pass --no-zip anywhere in the args to skip zipping (T3 signs the raw apphost before re-zip).
set -euo pipefail

cd "$(dirname "$0")"

PROJECT_FILE="src/com.IvanMurzak.Unreal.MCP.Bridge.csproj"
PUBLISH_ROOT="./publish"

# The 4 RIDs the bundle model ships (§6).
ALL_RUNTIMES=("win-x64" "linux-x64" "osx-x64" "osx-arm64")

CONFIGURATION="Release"
NO_ZIP=0
SELECTED=()

# First non-flag arg is the configuration; remaining non-flag args filter the RID list.
config_seen=0
for arg in "$@"; do
    case "$arg" in
        --no-zip)
            NO_ZIP=1
            ;;
        *)
            if [[ $config_seen -eq 0 ]]; then
                CONFIGURATION="$arg"
                config_seen=1
            else
                SELECTED+=("$arg")
            fi
            ;;
    esac
done

if [[ ${#SELECTED[@]} -gt 0 ]]; then
    RUNTIMES=("${SELECTED[@]}")
    # Validate each selected RID against the known set.
    for r in "${RUNTIMES[@]}"; do
        found=0
        for known in "${ALL_RUNTIMES[@]}"; do
            [[ "$r" == "$known" ]] && found=1
        done
        if [[ $found -eq 0 ]]; then
            echo "Unknown RID '$r'. Available: ${ALL_RUNTIMES[*]}" >&2
            exit 1
        fi
    done
else
    RUNTIMES=("${ALL_RUNTIMES[@]}")
fi

mkdir -p "$PUBLISH_ROOT"

for runtime in "${RUNTIMES[@]}"; do
    echo "Publishing $runtime ($CONFIGURATION)..."
    out="$PUBLISH_ROOT/$runtime"
    rm -rf "$out"
    dotnet publish "$PROJECT_FILE" -c "$CONFIGURATION" -r "$runtime" \
        --self-contained true -p:PublishSingleFile=true -o "$out"
    if [[ $NO_ZIP -eq 0 ]]; then
        (cd "$out" && zip -r "../unreal-mcp-bridge-$runtime.zip" .)
        echo "Created $PUBLISH_ROOT/unreal-mcp-bridge-$runtime.zip"
    fi
done

echo "All bridge publishes completed. Artifacts in: $PUBLISH_ROOT"
