# E2E tool check (one-test-per-tool). Round-trips enhanced-input-add-mapping through the live MCP
# server. Asset-independent: a non-existent contextPath passes schema validation but the handler's
# defensive branch rejects it after the AssetRegistry existence check — so the round-trip is exercised
# WITHOUT seeding a .uasset (a real mapping needs an IMC + IA asset, validated by the Automation spec
# + a live smoke).
@{
    Tool        = "enhanced-input-add-mapping"
    System      = $false
    Input       = '{"contextPath":"/Game/__DoesNotExist_AIEIE2E__/IMC","actionPath":"/Game/__DoesNotExist_AIEIE2E__/IA","key":"SpaceBar"}'
    ExpectError = $true
    Assert      = {
        param($Result)
        $serialized = $Result | ConvertTo-Json -Depth 20 -Compress
        if ($serialized -notmatch 'No Mapping Context found') {
            throw "expected a 'No Mapping Context found' error; got: $serialized"
        }
    }
}
