# E2E tool check (one-test-per-tool). Round-trips landscape-get-water-body through the live MCP
# server. Asset-independent: a non-existent actor name passes schema validation but the handler's
# defensive branch rejects it after resolving the editor world — so the round-trip and the
# game-thread world access are both exercised WITHOUT seeding a Water body (a real lookup needs an
# authored level with the Water plugin).
@{
    Tool        = "landscape-get-water-body"
    System      = $false
    Input       = '{"name":"__DoesNotExist_AILandscapeE2E__"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Water body named') {
            throw "expected a 'No Water body named' error; got: $serialized"
        }
    }
}
