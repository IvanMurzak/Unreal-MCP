# UnrealAITemplate (placeholder)

This folder will contain **UnrealAITemplate** — a minimal standalone UE plugin demonstrating the
Unreal-MCP **extensions** contract (`IUnrealMcpToolProvider`, registered through `IModularFeatures`
under the feature name `UnrealMcpToolProvider`): one `hello-extension` tool proving the contract
end-to-end. It doubles as the extension-author documentation's working example and as the
error-isolation test fixture (it can be switched to emit an invalid schema).

See [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §5 (extensions mechanism). The real
template lands with the extensions task — only this placeholder exists for now.
