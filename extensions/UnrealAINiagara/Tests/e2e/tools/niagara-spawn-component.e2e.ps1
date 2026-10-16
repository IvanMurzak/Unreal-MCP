# E2E tool check (one-test-per-tool). Round-trips niagara-spawn-component through the live MCP
# server. Asset-independent: a non-existent systemPath passes schema validation but the handler's
# defensive branch rejects it AFTER resolving GEditor + the editor world — so the round-trip and
# the game-thread world access are both exercised without seeding a .uasset (a real spawn needs a
# Niagara system asset + a rendering context, validated by the Automation spec + a live smoke).
@{
    Tool        = "niagara-spawn-component"
    System      = $false
    Input       = '{"systemPath":"/Game/__DoesNotExist_AINiagaraE2E__","location":{"x":0,"y":0,"z":0}}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Niagara system found') {
            throw "expected a 'No Niagara system found' error; got: $serialized"
        }
    }
}
