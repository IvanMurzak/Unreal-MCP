# E2E tool check (one-test-per-tool). Round-trips enhanced-input-get through the live MCP server.
# Asset-independent: we point at a path that cannot exist, so the DEFENSIVE branch runs and the
# full CLI -> server -> bridge -> handler -> back path is exercised without seeding a .uasset.
@{
    Tool        = "enhanced-input-get"
    System      = $false
    Input       = '{"path":"/Game/__DoesNotExist_AIEnhancedInputE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        # The handler returns a well-formed Error naming the missing asset. Assert that error text
        # round-tripped back (tolerant of the exact REST envelope / isError shape).
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No asset found') {
            throw "expected a 'No asset found' error; got: $serialized"
        }
    }
}
