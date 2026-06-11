// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/PlatformTLS.h"
#include "Dom/JsonObject.h"

#include <atomic>

class FUnrealMcpToolRegistry;
class FUnrealMcpGameThreadDispatcher;
class FTcpListener;
class FSocket;
class FRunnableThread;
struct FIPv4Endpoint;

/**
 * The plugin-side IPC bridge server (docs/ARCHITECTURE.md §1). Listens on 127.0.0.1 at a deterministic
 * per-project port (§1.1), accepts the sidecar's dial, validates the stdin-delivered token in the
 * handshake (§1.4), then exchanges NDJSON messages (§1.2): pushes the tool manifest + config on Ready,
 * routes tool-calls through the game-thread dispatcher (§4), and answers heartbeats (§1.3).
 *
 * Threading: FTcpListener accepts on its own thread; a dedicated reader thread (this FRunnable) drains
 * the connection; a heartbeat thread pings on a timer. All socket WRITES go through a single mutex so
 * messages never interleave (§1.2). Tool bodies run on the game thread via the dispatcher — never here.
 */
class FUnrealMcpBridgeServer : public FRunnable
{
public:
	FUnrealMcpBridgeServer(FUnrealMcpToolRegistry& InRegistry, FUnrealMcpGameThreadDispatcher& InDispatcher);
	virtual ~FUnrealMcpBridgeServer() override;

	/**
	 * Bind + listen on the deterministic port for @p ProjectPath (probing forward on conflict, §1.1) and
	 * begin accepting. Returns the bound port, or -1 if every probed port failed. @p Token is the secret
	 * the handshake must carry (§1.4); the other args are echoed in the handshake-ack. @p InEffectiveConfig
	 * is the resolved connection config (§8 — mode/host/cloudUrl/token/keepConnected) the plugin pushes to
	 * the sidecar in the handshake-ack and the §1.3 `config` message; may be null (the sidecar then keeps
	 * its env fallback).
	 */
	int32 Start(const FString& InToken, const FString& InProjectPath, const FString& InPluginVersion, const FString& InEngineVersion,
		const TSharedPtr<FJsonObject>& InEffectiveConfig = nullptr);

	/** Stop accepting, send the connected sidecar a graceful shutdown, and tear down all threads/sockets. */
	void Shutdown();

	int32 GetBoundPort() const { return BoundPort; }
	bool IsClientConnected() const { return bClientConnected; }

	/** Re-push the manifest (call after the registry changes, §2.2 hot reload). No-op when disconnected. */
	void PushManifest();

	/**
	 * Replace the effective connection config and, if a sidecar is connected, push the §1.3 `config` message
	 * to it (the "on change" path — e.g. a future UI mode/host/token edit). Thread-safe; no-op send when
	 * disconnected (the next handshake-ack carries the latest config anyway).
	 */
	void SetEffectiveConfig(const TSharedPtr<FJsonObject>& InEffectiveConfig);

	/** Push the current §1.3 `config` message to the connected sidecar. No-op when disconnected. */
	void PushConfig();

	/**
	 * Send a §1.3 auth message (`auth-start` / `auth-cancel` / `auth-revoke`) to the connected sidecar
	 * (the §7 Cloud device-code controls). Thread-safe; no-op (returns false) when disconnected. The UI
	 * calls this on the game thread; the frame is serialized through the shared WriteMutex like any other.
	 */
	bool SendAuthMessage(const FString& AuthType);

	/**
	 * Register a sink for the inbound sidecar→plugin `status` and `device-auth` feed (§1.3 / §7). The sink
	 * is invoked ON THE IPC READER THREAD with the message type and parsed object — the caller (the §7
	 * view-model) MUST marshal onto the game thread (AsyncTask(GameThread)) before touching any Slate state
	 * (the M9b main-thread-marshalled-subscriptions rule). Pass an unbound TFunction to clear it (window
	 * close). Thread-safe; the previous sink is replaced.
	 */
	void SetStatusSink(TFunction<void(const FString& /*Type*/, TSharedPtr<FJsonObject> /*Message*/)> InSink);

	/** Deterministic IPC port for a project path (§1.1): 30000 + sha(path) % 10000. */
	static int32 ComputeDeterministicPort(const FString& ProjectPath);

	// FRunnable (the reader loop) ------------------------------------------------------------------
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	bool HandleConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& Endpoint);
	void HandleLine(const FString& Line, int32 Generation);
	void HandleHandshake(const TSharedPtr<FJsonObject>& Message, int32 Generation);
	void HandleToolCall(const TSharedPtr<FJsonObject>& Message);
	void HandleToolCancel(const TSharedPtr<FJsonObject>& Message);

	bool SendMessage(const TSharedPtr<FJsonObject>& Message);
	void SendHandshakeAck();
	void SendManifestLocked();
	void SendConfigLocked(); // WriteMutex held: frame + send the §1.3 `config` message (no-op when no config)
	bool TrySendFramedLocked(const TArray<uint8>& Framed); // WriteMutex+ConnectionMutex held, ClientSocket valid
	void CloseActiveConnection();
	void DrainPendingDestroy();    // reader-/shutdown-owned: actually frees sockets parked for teardown
	void StartHeartbeat();
	void StopHeartbeat();
	void CancelAllInFlight();                      // set every in-flight call's cancel flag (shutdown)
	void DrainInFlightCalls(FTimespan Grace);      // wait (bounded) for in-flight continuations to finish

	FUnrealMcpToolRegistry& Registry;
	FUnrealMcpGameThreadDispatcher& Dispatcher;

	FTcpListener* Listener = nullptr;
	FSocket* ListenSocket = nullptr;       // owned by this (FTcpListener uses it but does not delete it)
	FSocket* ClientSocket = nullptr;       // the accepted sidecar connection (owned by this; guarded by ConnectionMutex)
	FRunnableThread* ReaderThread = nullptr;

	// Sockets parked for teardown. The accept thread / a failed send / a heartbeat drop only PARK the old
	// socket here (under ConnectionMutex); the actual Close+DestroySocket happens on the reader thread (or
	// in Shutdown after the reader is dead) via DrainPendingDestroy() — never while another thread might
	// still be mid-Recv on it. This is the generation-based fix for the second-connection use-after-free.
	TArray<FSocket*> SocketsPendingDestroy;
	int32 ConnectionGeneration = 0;        // bumps on every accepted connection (guarded by ConnectionMutex)
	double ConnectionAcceptedSeconds = 0.0; // wall-clock of the current accept (for the pre-handshake deadline)

	FString Token;
	FString ProjectPath;
	FString PluginVersion;
	FString EngineVersion;
	int32 BoundPort = -1;

	// The resolved §8 connection config pushed to the sidecar (handshake-ack + §1.3 `config`). Read on the
	// reader thread (SendHandshakeAck/SendConfigLocked), replaced on the game thread (SetEffectiveConfig);
	// guarded by ConfigMutex. A deep copy is stored so the caller's object can change without racing a send.
	mutable FCriticalSection ConfigMutex;
	TSharedPtr<FJsonObject> EffectiveConfig;

	// Sink for the inbound `status` / `device-auth` feed (§1.3 / §7). Set/cleared on the game thread (window
	// open/close), invoked on the reader thread; guarded so a window-close clear cannot race a reader invoke.
	mutable FCriticalSection StatusSinkMutex;
	TFunction<void(const FString&, TSharedPtr<FJsonObject>)> StatusSink;

	FThreadSafeBool bStopRequested = false;
	FThreadSafeBool bClientConnected = false;
	FThreadSafeBool bHandshakeOk = false;
	FThreadSafeCounter LastActivitySeconds; // wall-clock seconds of last inbound activity

	mutable FCriticalSection WriteMutex;     // serializes all socket sends (§1.2)
	mutable FCriticalSection ConnectionMutex; // guards ClientSocket swap/teardown

	// In-flight cancellation flags, keyed by requestId (§4).
	FCriticalSection CancelMutex;
	TMap<FString, TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe>> CancelFlags;
	FThreadSafeCounter InFlightCalls;      // dispatched calls whose continuation has not yet completed

	TFuture<void> HeartbeatFuture;
	FThreadSafeBool bHeartbeatStop = false;
	std::atomic<uint32> HeartbeatThreadId{ 0 }; // id of the live heartbeat thread, for the self-join guard
};
