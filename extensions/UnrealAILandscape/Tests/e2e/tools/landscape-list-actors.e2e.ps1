# E2E tool check (one-test-per-tool). Returned to Run-ToolChecks.ps1, which invokes
# `unreal-mcp-cli run-tool landscape-list-actors` against the running project's MCP server and
# asserts a well-formed success. Asset-independent: an empty world returns count 0.
@{
    Tool   = "landscape-list-actors"
    System = $false
    Input  = '{}'
    Assert = {
        param($Result)
        # The tool returns a structured result carrying { count, actors }. Assert the shape is
        # present (a well-formed success), tolerant of the exact REST envelope.
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'actors') {
            throw "expected an 'actors' field in the tool result; got: $serialized"
        }
    }
}
