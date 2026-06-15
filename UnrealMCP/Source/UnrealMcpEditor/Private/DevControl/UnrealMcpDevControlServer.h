// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "HttpRouteHandle.h"   // FHttpRouteHandle is a TSharedPtr typedef — cannot be forward-declared.
#include "HttpResultCallback.h" // FHttpResultCallback is a TFunction typedef — cannot be forward-declared.

class FUnrealMcpEditorViewModel;
class FJsonObject;
class IHttpRouter;
struct FHttpServerRequest;

/**
 * DEV-ONLY inject/control HTTP bridge over the live "AI Game Developer" Slate dock (docs/ARCHITECTURE.md
 * §4/§7). A sibling of the Godot-MCP dev-control bridge: it lets a terminal / AI agent INJECT fake states
 * into the dock view-model and CONTROL it (simulate the same button actions the Slate widgets fire), for
 * tests + AI-driven dev. NOT a product feature — it never ships listening.
 *
 * Security / scope (NON-NEGOTIABLE, mirrored from the design spec):
 *  - Started ONLY when the editor process env `UNREAL_MCP_DEV_CONTROL == "1"`; OFF by default, so a
 *    shipped plugin never opens a port. The port comes from `UNREAL_MCP_DEV_CONTROL_PORT` (default 9921).
 *  - UE's FHttpServerModule binds all interfaces, so every handler additionally verifies the request's
 *    PeerAddress is loopback (127.0.0.1 / ::1) and rejects anything else — defence in depth behind the
 *    env gate. The port is documented loopback-dev-only.
 *  - Handlers run on the GAME THREAD (FHttpServerModule ticks there), so they call the view-model directly
 *    with no extra marshalling — the view-model is game-thread-only and the dock reads it on the same thread.
 *
 * The server holds the live view-model weakly (the runtime owns it); a request that straddles teardown is a
 * clean 503, never a use-after-free. Start(Port)/Stop() are idempotent and game-thread only.
 */
class FUnrealMcpDevControlServer
{
public:
	FUnrealMcpDevControlServer();
	~FUnrealMcpDevControlServer();

	/**
	 * Bind the HTTP router on @p Port and register the v1 routes. @p InViewModel is the live dock view-model
	 * (held weakly). Returns true when the router bound and routes registered. Idempotent: a second Start is
	 * a no-op. Logs `[dev-control] Listening on http://127.0.0.1:<port>` on success.
	 */
	bool Start(uint32 Port, const TSharedPtr<FUnrealMcpEditorViewModel>& InViewModel);

	/** Unbind every route, drop the router, and forget the view-model. Idempotent. */
	void Stop();

	bool IsRunning() const { return bRunning; }
	uint32 GetPort() const { return BoundPort; }

	/**
	 * The PURE request → view-model router (the spec-friendly heart, no live HTTP socket). Applies @p Method
	 * + @p Path + parsed @p Body to @p ViewModel and writes a JSON result object into @p OutResult
	 * (`{"ok":true,...}` / `{"ok":false,"error":...}`). Returns the HTTP status code the handler would send
	 * (200 / 400 / 404 / 503). @p ViewModel may be null (→ 503). Static + side-effect-confined to the
	 * view-model so the UnrealMcpEditorTests spec drives it directly.
	 */
	// UNREALMCPEDITOR_API-exported so the UnrealMcpEditorTests module (a separate DLL) links this static router.
	static UNREALMCPEDITOR_API int32 RouteRequest(
		const FString& Method,
		const FString& Path,
		const TSharedPtr<FJsonObject>& Body,
		FUnrealMcpEditorViewModel* ViewModel,
		TSharedRef<FJsonObject>& OutResult);

private:
	// The single FHttpRequestHandler body, shared by every bound route: verifies loopback, parses the JSON
	// body, resolves the view-model, calls RouteRequest, and writes the JSON response. The bound @p Method +
	// @p Path are captured per-route (NOT read from the request — FHttpServerRequest::RelativePath is route-
	// relative, so it is "/" for an exact match). Returns true (always completes synchronously on the game thread).
	bool HandleRequest(const FString& Method, const FString& Path, const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	TWeakPtr<FUnrealMcpEditorViewModel> ViewModel;
	uint32 BoundPort = 0;
	bool bRunning = false;
};
