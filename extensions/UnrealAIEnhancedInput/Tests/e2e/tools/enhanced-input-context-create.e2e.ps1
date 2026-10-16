# E2E tool check (one-test-per-tool). Round-trips enhanced-input-context-create through the live MCP
# server. Asset-independent: we target an ENGINE content root so the handler's defensive write-root
# guard rejects it AFTER splitting the path — the full round-trip is exercised WITHOUT writing any
# asset into /Game (a real create is round-trip-validated by the Automation spec + a live smoke).
@{
    Tool        = "enhanced-input-context-create"
    System      = $false
    Input       = '{"path":"/Engine/__AIEnhancedInputE2E__/IMC_Probe"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'engine content root') {
            throw "expected an 'engine content root' refusal error; got: $serialized"
        }
    }
}
