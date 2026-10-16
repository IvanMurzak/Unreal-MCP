# E2E tool check (one-test-per-tool). Round-trips landscape-get-actor through the live MCP server.
# Asset-independent: a non-existent actor name passes schema validation but the handler's defensive
# branch rejects it after resolving the editor world — so the round-trip and the game-thread world
# access are both exercised WITHOUT seeding a Landscape (a real lookup needs an authored level).
@{
    Tool        = "landscape-get-actor"
    System      = $false
    Input       = '{"name":"__DoesNotExist_AILandscapeE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Landscape actor named') {
            throw "expected a 'No Landscape actor named' error; got: $serialized"
        }
    }
}
