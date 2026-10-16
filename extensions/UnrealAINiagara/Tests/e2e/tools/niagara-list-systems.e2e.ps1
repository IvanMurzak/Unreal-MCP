# E2E tool check (one-test-per-tool). Returned to Run-ToolChecks.ps1, which invokes
# `unreal-mcp-cli run-tool niagara-list-systems` against the running project's MCP server and
# asserts a well-formed success. Asset-independent: an empty project returns count 0.
@{
    Tool   = "niagara-list-systems"
    System = $false
    Input  = '{}'
    Assert = {
        param($Result)
        # The tool returns a structured result carrying { count, systems }. Assert the shape is
        # present (a well-formed success), tolerant of the exact REST envelope.
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'systems') {
            throw "expected a 'systems' field in the tool result; got: $serialized"
        }
    }
}
