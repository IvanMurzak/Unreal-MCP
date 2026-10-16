# E2E tool check (one-test-per-tool). Round-trips gas-inspect-ability through the live MCP server.
# Asset-independent: we point at a class path that cannot exist, so the DEFENSIVE branch runs and the
# full CLI -> server -> bridge -> handler -> back path is exercised without seeding a Gameplay Ability
# blueprint (a real inspect needs an ability class, validated by the Automation spec + a live smoke).
@{
    Tool        = "gas-inspect-ability"
    System      = $false
    Input       = '{"path":"/Game/__DoesNotExist_AIGASE2E__.__DoesNotExist_AIGASE2E___C"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        # The handler returns a well-formed Error naming the missing ability class. Assert that error
        # text round-tripped back (tolerant of the exact REST envelope / isError shape).
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Gameplay Ability class found') {
            throw "expected a 'No Gameplay Ability class found' error; got: $serialized"
        }
    }
}
