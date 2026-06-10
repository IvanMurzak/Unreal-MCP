#!/usr/bin/env bash
# Publish self-contained single-file unreal-mcp-bridge binaries per RID and zip them
# (docs/ARCHITECTURE.md §6: unreal-mcp-bridge-<platform>.zip release artifacts).
set -euo pipefail

cd "$(dirname "$0")"

CONFIGURATION="${1:-Release}"
PROJECT_FILE="src/com.IvanMurzak.Unreal.MCP.Bridge.csproj"
PUBLISH_ROOT="./publish"

# The 4 RIDs the sidecar download flow serves (§6).
RUNTIMES=("win-x64" "linux-x64" "osx-x64" "osx-arm64")

rm -rf "$PUBLISH_ROOT"
mkdir -p "$PUBLISH_ROOT"

for runtime in "${RUNTIMES[@]}"; do
    echo "Publishing $runtime..."
    out="$PUBLISH_ROOT/$runtime"
    dotnet publish "$PROJECT_FILE" -c "$CONFIGURATION" -r "$runtime" \
        --self-contained true -p:PublishSingleFile=true -o "$out"
    (cd "$out" && zip -r "../unreal-mcp-bridge-$runtime.zip" .)
    echo "Created $PUBLISH_ROOT/unreal-mcp-bridge-$runtime.zip"
done

echo "All bridge publishes completed. Artifacts in: $PUBLISH_ROOT"
