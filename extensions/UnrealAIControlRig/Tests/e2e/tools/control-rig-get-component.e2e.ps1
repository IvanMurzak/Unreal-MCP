# E2E tool check (one-test-per-tool). Round-trips control-rig-get-component through the live MCP
# server. Asset-independent: a non-existent actor label triggers the DEFENSIVE not-found branch
# AFTER resolving GEditor + the editor world, so the round-trip and the game-thread world access
# are both exercised without seeding an actor (a real inspection needs an actor carrying a Control
# Rig component, validated by the Automation spec + a live smoke).
@{
    Tool        = "control-rig-get-component"
    System      = $false
    Input       = '{"actorName":"__DoesNotExist_AIControlRigE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No actor named') {
            throw "expected a 'No actor named' error; got: $serialized"
        }
    }
}
