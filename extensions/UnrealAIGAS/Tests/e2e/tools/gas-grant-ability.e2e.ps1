# E2E tool check (one-test-per-tool). Round-trips gas-grant-ability through the live MCP server.
# Asset-independent: a non-existent actor name passes schema validation but the handler's defensive
# branch rejects it AFTER resolving GEditor + the editor world — so the round-trip and the
# game-thread world access are both exercised without seeding an ASC-bearing actor (a real grant
# needs an actor that owns an AbilitySystemComponent + a Gameplay Ability class, validated by the
# Automation spec + a live smoke).
@{
    Tool        = "gas-grant-ability"
    System      = $false
    Input       = '{"actor":"__DoesNotExist_AIGASE2E__","abilityClass":"/Game/__DoesNotExist_AIGASE2E__.__DoesNotExist_AIGASE2E___C"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No actor named') {
            throw "expected a 'No actor named' error; got: $serialized"
        }
    }
}
