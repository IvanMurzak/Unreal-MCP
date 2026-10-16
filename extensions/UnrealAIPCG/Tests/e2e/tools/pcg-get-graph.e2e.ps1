# E2E tool check (one-test-per-tool). Round-trips pcg-get-graph through the live MCP server.
# Asset-independent: we point at a path that cannot exist, so the DEFENSIVE branch runs and the
# full CLI -> server -> bridge -> handler -> back path is exercised without seeding a .uasset.
@{
    Tool        = "pcg-get-graph"
    System      = $false
    Input       = '{"path":"/Game/__DoesNotExist_AIPCGE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        # The handler returns a well-formed Error naming the missing graph. Assert that error text
        # round-tripped back (tolerant of the exact REST envelope / isError shape).
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No PCG graph found') {
            throw "expected a 'No PCG graph found' error; got: $serialized"
        }
    }
}
