# E2E tool check (one-test-per-tool). Round-trips pcg-get-component through the live MCP server.
# Asset-independent: a non-existent actorName passes schema validation but the handler's defensive
# branch rejects it AFTER resolving GEditor + the editor world — so the round-trip and the
# game-thread world access are both exercised without seeding an actor.
@{
    Tool        = "pcg-get-component"
    System      = $false
    Input       = '{"actorName":"__DoesNotExist_AIPCGE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No actor named') {
            throw "expected a 'No actor named' error; got: $serialized"
        }
    }
}
