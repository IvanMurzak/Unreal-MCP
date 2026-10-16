# E2E tool check (one-test-per-tool). Returned to Run-ToolChecks.ps1, which invokes
# `unreal-mcp-cli run-tool control-rig-list` against the running project's MCP server and asserts a
# well-formed success. Asset-independent: an empty project returns count 0.
@{
    Tool   = "control-rig-list"
    System = $false
    Input  = '{}'
    Assert = {
        param($Result)
        # The tool returns a structured result carrying { count, controlRigs }. Assert the shape is
        # present (a well-formed success), tolerant of the exact REST envelope.
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'controlRigs') {
            throw "expected a 'controlRigs' field in the tool result; got: $serialized"
        }
    }
}
