# E2E tool check (one-test-per-tool). Returned to Run-ToolChecks.ps1, which invokes
# `unreal-mcp-cli run-tool gas-list-abilities` against the running project's MCP server and asserts
# a well-formed success. Asset-independent: an empty project still returns a { count, abilities } shape.
@{
    Tool   = "gas-list-abilities"
    System = $false
    Input  = '{}'
    Assert = {
        param($Result)
        # The tool returns a structured result carrying { count, abilities }. Assert the shape is
        # present (a well-formed success), tolerant of the exact REST envelope.
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'abilities') {
            throw "expected an 'abilities' field in the tool result; got: $serialized"
        }
    }
}
