// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealMcpBridgeServer.h"

/**
 * IPC version negotiation (M16 P0, docs/ARCHITECTURE.md §A.1): the plugin↔sidecar handshake negotiates the
 * effective wire-protocol version as min(local, remote), so a v2 plugin paired with a v1 sidecar (or vice
 * versa) negotiates down to v1 and the link stays tools-only — an old peer keeps working, and the v2
 * prompt/resource families are simply never exchanged. Mirrors the bridge's IpcProtocolV2Tests so both ends
 * of the contract are pinned. Pure-function coverage of FUnrealMcpBridgeServer::NegotiateIpcVersion +
 * GetIpcVersion (no live socket).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpIpcVersionSpec, "UnrealMcp.IpcVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpIpcVersionSpec)

void FUnrealMcpIpcVersionSpec::Define()
{
	Describe("IPC version", [this]()
	{
		It("advertises v2 (the M16 P0 bump)", [this]()
		{
			TestEqual(TEXT("plugin IpcVersion"), FUnrealMcpBridgeServer::GetIpcVersion(), 2);
		});
	});

	Describe("version negotiation (old/new pair compatibility)", [this]()
	{
		It("negotiates v2 when both peers are v2 (prompts/resources enabled)", [this]()
		{
			TestEqual(TEXT("v2 x v2"), FUnrealMcpBridgeServer::NegotiateIpcVersion(2, 2), 2);
		});

		It("negotiates DOWN to v1 when the sidecar is an old v1 peer (tools-only; old peer keeps working)", [this]()
		{
			TestEqual(TEXT("v2 plugin x v1 sidecar"), FUnrealMcpBridgeServer::NegotiateIpcVersion(2, 1), 1);
		});

		It("negotiates DOWN to v1 when this plugin is older than the sidecar", [this]()
		{
			TestEqual(TEXT("v1 plugin x v2 sidecar"), FUnrealMcpBridgeServer::NegotiateIpcVersion(1, 2), 1);
		});

		It("negotiates v1 when both peers are v1", [this]()
		{
			TestEqual(TEXT("v1 x v1"), FUnrealMcpBridgeServer::NegotiateIpcVersion(1, 1), 1);
		});

		It("treats an absent/legacy remote version (0) as v1", [this]()
		{
			TestEqual(TEXT("v2 plugin x legacy handshake"), FUnrealMcpBridgeServer::NegotiateIpcVersion(2, 0), 1);
		});

		It("a future-newer sidecar still negotiates down to this plugin's version", [this]()
		{
			TestEqual(TEXT("v2 plugin x v3 sidecar"), FUnrealMcpBridgeServer::NegotiateIpcVersion(2, 3), 2);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
