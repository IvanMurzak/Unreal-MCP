# E2E tool check (one-test-per-tool). Round-trips control-rig-list-controls through the live MCP
# server. Asset-independent: a non-existent path triggers the DEFENSIVE not-found branch, so the
# round-trip is exercised without seeding a .uasset (a real listing needs a Control Rig blueprint
# asset, validated by the Automation spec).
@{
    Tool        = "control-rig-list-controls"
    System      = $false
    Input       = '{"path":"/Game/__DoesNotExist_AIControlRigE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Control Rig found') {
            throw "expected a 'No Control Rig found' error; got: $serialized"
        }
    }
}
