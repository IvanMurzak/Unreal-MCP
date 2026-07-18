# Device-flow smoke (editor Cloud Authorize) — sidecar parity + B14

This is the manual smoke that proves the Unreal editor Cloud sign-in reaches parity with Unity/Godot
**through the .NET sidecar** and that the machine credential store is definitely wired (auth-fixes **B14**,
verification **V11**). It exists because Unreal's device flow is unique: unlike the C# engines that host the
McpPlugin auth stack in-process, the Unreal **plugin** only relays `auth-start` over IPC and the
**sidecar** (`unreal-mcp-bridge`) runs the OAuth device-code flow.

## IPC surface (verified unchanged by the unified flow)

The plugin↔sidecar auth surface is **stable** — the auth-fixes redesign changed the server audience handling
(B11/c2) and the LIB dual-hash metadata (f1b), **none of which touch these IPC messages**:

| Direction | Message | Payload |
|---|---|---|
| plugin → sidecar | `auth-start` / `auth-cancel` / `auth-revoke` | a bare message `type` — **no `client_id`, no audience, no params** (`UnrealMcpEditorViewModel::Authorize` → `OnSendAuth("auth-start")`; `IpcProtocol.MessageTypes`) |
| sidecar → plugin | `device-auth` (feed) | `{ state: pending\|authorized\|failed, verificationUrl, userCode, token, message }` (`DeviceCodeAuthenticator` emits; `UnrealMcpEditorViewModel::ApplyDeviceAuth` consumes) |

The plugin **client_id stays `unreal-mcp-plugin`** and lives entirely **bridge-side**
(`DeviceCodeAuthenticator.DefaultClientId`), used only in the sidecar's `POST /oauth/device_authorization`
+ `POST /oauth/token` calls (form `client_id` + `scope=mcp:plugin`). It never crosses the IPC boundary, so
the unified flow needs **no IPC change** — this smoke confirms that end-to-end.

## Preconditions

- Local server stack from fresh `main` (skill `local-test`): backend + `mcp-server`, `MCP_AUTH=oauth`.
- `engines/unreal/test-project` open in UE 5.7 with the `Plugins/UnrealMCP` junction pointed at the plugin.
- Clean state between runs: remove `~/.ai-game-dev/credentials.json`, reset the testbed `.mcp.json`,
  revoke any live sessions.

## Smoke steps (V11)

1. **Open the AI Game Developer window** → select **Cloud** mode → click **Authorize**.
   - The ViewModel sends `auth-start` over IPC; if the sidecar is not yet handshaken it enters the transient
     **Connecting** state and flushes the queued `auth-start` on handshake (issue #99). Expect the state to
     advance to **Pending**.
2. **Device-code feed.** The sidecar streams a `device-auth` message carrying `verificationUrl` + `userCode`;
   the window shows them. Open the URL in a browser, sign in, and approve.
3. **Authorized.** The sidecar's poll succeeds; it emits `device-auth { state: authorized, token }` and — because
   prod always wires the store (B14, below) — **persists** the credential to the machine store and adopts it live.
   The window's cloud-auth indicator lights **Authorized**.
4. **Machine store wired (B14 check).** Confirm `~/.ai-game-dev/credentials.json` now exists and carries
   `accessToken` + `refreshToken` + `expiresAt` + `serverTarget` (the cloud base, `/mcp` stripped). This is the
   proof the sidecar did **not** silently degrade to a static bearer.
5. **Agent sees engine tools.** Point a real agent (Claude Code `.mcp.json` in the testbed) at the pinned URL and
   confirm `tools/list` returns the full engine toolset — not the 3 native tools.
6. **Silent refresh (ties to V10).** Shorten the access-token TTL locally (or wait for `exp`); the sidecar
   refreshes via the wired refresher on the next dial / on 401 with no user action — the connection stays live.

## B14 — the machine store is ALWAYS wired

The sidecar is built through `SidecarHost.CreateForProduction(...)` (the only path `Program.Main` uses), which
**unconditionally** wires the shared `MachineCredentialStore` + `OAuthTokenRefresher`. The raw `SidecarHost`
constructor still accepts a null store, but that overload is a **test-only** seam for the static/Custom-bearer
path — no production path reaches it. So a Cloud sign-in can never fall back to a static bearer without refresh
(the pre-B14 `if (credentialStore != null)` silent-degradation risk is removed).

Automated coverage lives in `bridge/tests/SidecarHostProdWiringTests.cs`:

- `CreateForProduction_AlwaysWiresCredentialStore_EvenWithNoExplicitStore` — the factory never yields an unwired
  sidecar (the exact shape a dropped-store regression would hit).
- `CreateForProduction_WithSeededStore_DrivesCloudBearerFromStore_NotStatic` — a seeded credential drives the
  Cloud bearer (zero-button boot).
- `CreateForProduction_RefreshPath_RotatesExpiredStoredCredential` — an expired stored credential is refreshed
  via the wired refresher before the connection layer receives a bearer.
