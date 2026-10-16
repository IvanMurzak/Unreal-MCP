# E2E tool check (one-test-per-tool). Returned to Run-ToolChecks.ps1, which invokes
# `unreal-mcp-cli run-tool gas-list-attribute-sets` against the running project's MCP server and
# asserts a well-formed success. Asset-independent: an empty project still returns a { count,
# attributeSets } shape.
@{
    Tool   = "gas-list-attribute-sets"
    System = $false
    Input  = '{}'
    Assert = {
        param($Result)
        # The tool returns a structured result carrying { count, attributeSets }. Assert the shape is
        # present (a well-formed success), tolerant of the exact REST envelope.
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'attributeSets') {
            throw "expected an 'attributeSets' field in the tool result; got: $serialized"
        }
    }
}
