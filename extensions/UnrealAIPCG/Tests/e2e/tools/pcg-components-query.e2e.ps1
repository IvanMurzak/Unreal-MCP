# E2E check of the extension's RQL module (Unreal-MCP docs/rql.md). Returned to scripts/run-extension-e2e.ps1, which
# invokes `unreal-mcp-cli run-tool rql-query` against the running project's MCP server and asserts a well-formed success.
@{
    Tool   = "rql-query"
    System = $false
    Input  = '{"query":"actors | get Components | where $ is PCGComponent | count"}'
    Assert = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch '"text"\s*:\s*"\d+"|value\\?"\s*:\s*\d') {
            throw "expected the count of PCG components; got: $serialized"
        }
    }
}
