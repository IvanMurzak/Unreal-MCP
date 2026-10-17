# E2E check of the extension's RQL module (Unreal-MCP docs/rql.md). Returned to scripts/run-extension-e2e.ps1, which
# invokes `unreal-mcp-cli run-tool rql-schema` against the running project's MCP server and asserts a well-formed success.
@{
    Tool   = "rql-schema"
    System = $false
    Input  = '{"type":"GASOperations"}'
    Assert = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'GrantAbility') {
            throw "expected GrantAbility among the operations; got: $serialized"
        }
    }
}
