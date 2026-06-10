// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Dom/JsonObject.h"

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
	 * the handshake must carry (§1.4); the other args are echoed in the handshake-ack.
	 */
	int32 Start(const FString& InToken, const FString& InProjectPath, const FString& InPluginVersion, const FString& InEngineVersion);

	/** Stop accepting, send the connected sidecar a graceful shutdown, and tear down all threads/sockets. */
	void Shutdown();

	int32 GetBoundPort() const { return BoundPort; }
	bool IsClientConnected() const { return bClientConnected; }

	/** Re-push the manifest (call after the registry changes, §2.2 hot reload). No-op when disconnected. */
	void PushManifest();

	/** Deterministic IPC port for a project path (§1.1): 30000 + sha(path) % 10000. */
	static int32 ComputeDeterministicPort(const FString& ProjectPath);

	// FRunnable (the reader loop) ------------------------------------------------------------------
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	bool HandleConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& Endpoint);
	void HandleLine(const FString& Line);
	void HandleHandshake(const TSharedPtr<FJsonObject>& Message);
	void HandleToolCall(const TSharedPtr<FJsonObject>& Message);
	void HandleToolCancel(const TSharedPtr<FJsonObject>& Message);

	bool SendMessage(const TSharedPtr<FJsonObject>& Message);
	void SendHandshakeAck();
	void SendManifestLocked();
	void CloseActiveConnection();
	void StartHeartbeat();
	void StopHeartbeat();

	FUnrealMcpToolRegistry& Registry;
	FUnrealMcpGameThreadDispatcher& Dispatcher;

	FTcpListener* Listener = nullptr;
	FSocket* ListenSocket = nullptr;       // owned by this (FTcpListener uses it but does not delete it)
	FSocket* ClientSocket = nullptr;       // the accepted sidecar connection (owned by this)
	FRunnableThread* ReaderThread = nullptr;

	FString Token;
	FString ProjectPath;
	FString PluginVersion;
	FString EngineVersion;
	int32 BoundPort = -1;

	FThreadSafeBool bStopRequested = false;
	FThreadSafeBool bClientConnected = false;
	FThreadSafeBool bHandshakeOk = false;
	FThreadSafeCounter LastActivitySeconds; // wall-clock seconds of last inbound activity

	mutable FCriticalSection WriteMutex;     // serializes all socket sends (§1.2)
	mutable FCriticalSection ConnectionMutex; // guards ClientSocket swap/teardown

	// In-flight cancellation flags, keyed by requestId (§4).
	FCriticalSection CancelMutex;
	TMap<FString, TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe>> CancelFlags;

	TFuture<void> HeartbeatFuture;
	FThreadSafeBool bHeartbeatStop = false;
};
