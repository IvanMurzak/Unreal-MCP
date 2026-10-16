# E2E tool check (one-test-per-tool). Round-trips control-rig-get through the live MCP server.
# Asset-independent: we point at a path that cannot exist, so the DEFENSIVE not-found branch runs
# and the full CLI -> server -> bridge -> handler -> back path is exercised without seeding a
# .uasset (a real inspection needs a Control Rig blueprint asset, validated by the Automation spec).
@{
    Tool        = "control-rig-get"
    System      = $false
    Input       = '{"path":"/Game/__DoesNotExist_AIControlRigE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        # The handler returns a well-formed Error naming the missing Control Rig. Assert that error
        # text round-tripped back (tolerant of the exact REST envelope / isError shape).
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Control Rig found') {
            throw "expected a 'No Control Rig found' error; got: $serialized"
        }
    }
}
