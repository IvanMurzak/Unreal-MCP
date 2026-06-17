// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpBridgeServer.h"
#include "UnrealMcpNdjson.h"
#include "UnrealMcpToolRegistry.h"
#include "Dispatch/UnrealMcpGameThreadDispatcher.h"
#include "UnrealMcpLog.h"

#include "Common/TcpListener.h"
#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "HAL/RunnableThread.h"
#include "Async/Async.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace
{
	const FString TypeHandshake = TEXT("handshake");
	const FString TypeHandshakeAck = TEXT("handshake-ack");
	const FString TypeToolManifest = TEXT("tool-manifest");
	const FString TypeToolCall = TEXT("tool-call");
	const FString TypeToolResponse = TEXT("tool-response");
	const FString TypeToolCancel = TEXT("tool-cancel");
	const FString TypeConfig = TEXT("config");
	const FString TypeStatus = TEXT("status");
	const FString TypeDeviceAuth = TEXT("device-auth");
	const FString TypeAuthStart = TEXT("auth-start");
	const FString TypeAuthCancel = TEXT("auth-cancel");
	const FString TypeAuthRevoke = TEXT("auth-revoke");
	// §7 AI-agent configurator IPC verbs (plugin → sidecar requests + the sidecar → plugin result).
	const FString TypeAgentsList = TEXT("agents-list");
	const FString TypeAgentStatus = TEXT("agent-status");
	const FString TypeAgentConfigure = TEXT("agent-configure");
	const FString TypeAgentRemove = TEXT("agent-remove");
	const FString TypeAgentSkillsPath = TEXT("agent-skills-path");
	const FString TypeAgentConfigResult = TEXT("agent-config-result");
	const FString TypePing = TEXT("ping");
	const FString TypePong = TEXT("pong");
	const FString TypeShutdown = TEXT("shutdown");

	constexpr int32 IpcVersion = 1;
	constexpr int32 HeartbeatIntervalSeconds = 5;
	constexpr int32 HeartbeatTimeoutSeconds = 15;
	constexpr int32 DefaultToolTimeoutMs = 30000;

	FString SerializeCondensed(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}

	// Deep-copy a JSON object via a serialize/parse round-trip so the stored EffectiveConfig is fully
	// independent of the caller's object (the reader thread serializes it without holding the caller still).
	// Falls back to the original pointer if the round-trip fails (a flat config object never does).
	TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& In)
	{
		if (!In.IsValid())
			return nullptr;

		const FString Serialized = SerializeCondensed(In);
		TSharedPtr<FJsonObject> Out;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Serialized);
		if (FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid())
			return Out;
		return In;
	}
}

FUnrealMcpBridgeServer::FUnrealMcpBridgeServer(FUnrealMcpToolRegistry& InRegistry, FUnrealMcpGameThreadDispatcher& InDispatcher)
	: Registry(InRegistry)
	, Dispatcher(InDispatcher)
{
}

FUnrealMcpBridgeServer::~FUnrealMcpBridgeServer()
{
	Shutdown();
}

int32 FUnrealMcpBridgeServer::ComputeDeterministicPort(const FString& InProjectPath)
{
	const FString Normalized = InProjectPath.Replace(TEXT("\\"), TEXT("/"));
	const FString Seed = FString(TEXT("unreal-mcp-ipc:")) + Normalized;

	FSHA1 Sha;
	const FTCHARToUTF8 Utf8(*Seed);
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Digest[20];
	Sha.GetHash(Digest);

	const uint32 Value =
		(static_cast<uint32>(Digest[0]) << 24) |
		(static_cast<uint32>(Digest[1]) << 16) |
		(static_cast<uint32>(Digest[2]) << 8) |
		(static_cast<uint32>(Digest[3]));
	return 30000 + static_cast<int32>(Value % 10000);
}

int32 FUnrealMcpBridgeServer::Start(const FString& InToken, const FString& InProjectPath, const FString& InPluginVersion, const FString& InEngineVersion,
	const TSharedPtr<FJsonObject>& InEffectiveConfig)
{
	Token = InToken;
	ProjectPath = InProjectPath;
	PluginVersion = InPluginVersion;
	EngineVersion = InEngineVersion;
	{
		FScopeLock Lock(&ConfigMutex);
		EffectiveConfig = CloneJsonObject(InEffectiveConfig);
	}

	const int32 BasePort = ComputeDeterministicPort(ProjectPath);

	// §1.1: probe BasePort … BasePort+9, then an ephemeral port (0) as a last resort.
	for (int32 Offset = 0; Offset <= 10; ++Offset)
	{
		int32 TryPort;
		if (Offset < 10)
		{
			TryPort = 30000 + ((BasePort - 30000 + Offset) % 10000);
		}
		else
		{
			TryPort = 0; // ephemeral
		}

		FSocket* Candidate = FTcpSocketBuilder(TEXT("UnrealMcpIpcListen"))
			.AsNonBlocking()
			.BoundToAddress(FIPv4Address(127, 0, 0, 1))
			.BoundToPort(TryPort)
			.Listening(8)
			.Build();

		if (Candidate != nullptr)
		{
			ListenSocket = Candidate;
			// For an ephemeral bind, read back the actually-assigned port.
			TSharedRef<FInternetAddr> LocalAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
			ListenSocket->GetAddress(*LocalAddr);
			BoundPort = LocalAddr->GetPort();
			break;
		}
	}

	if (ListenSocket == nullptr)
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("[Unreal-MCP] bridge could not bind any IPC port near %d."), BasePort);
		return -1;
	}

	// FSocket& ctor: the listener does NOT own the socket (bDeleteSocket=false) — we destroy it ourselves.
	Listener = new FTcpListener(*ListenSocket, FTimespan::FromMilliseconds(200), false);
	Listener->OnConnectionAccepted().BindRaw(this, &FUnrealMcpBridgeServer::HandleConnectionAccepted);

	ReaderThread = FRunnableThread::Create(this, TEXT("UnrealMcpBridgeReader"), 0, TPri_Normal);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] bridge listening on 127.0.0.1:%d (project '%s')."), BoundPort, *ProjectPath);
	return BoundPort;
}

bool FUnrealMcpBridgeServer::HandleConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& Endpoint)
{
	if (InSocket == nullptr)
		return false;

	InSocket->SetNonBlocking(true);

	// One connection at a time: a fresh dial replaces a stale one (§1.5 re-dial after reconnect). We run
	// on the FTcpListener's accept thread, which must NEVER free the old socket: the reader thread may be
	// mid-Recv on it (the second-connection use-after-free). Park the old socket for the reader to destroy
	// and bump the connection generation so the reader resets its NDJSON accumulator and discards any bytes
	// it reads from the now-superseded socket.
	{
		FScopeLock Lock(&ConnectionMutex);
		if (ClientSocket != nullptr)
			SocketsPendingDestroy.Add(ClientSocket);
		ClientSocket = InSocket;
		++ConnectionGeneration;
		ConnectionAcceptedSeconds = FPlatformTime::Seconds();

		// Reset the connection flags INSIDE the lock so the socket swap, the generation bump, and the flag
		// reset are one atomic step. If these writes happened after releasing the lock, the reader could
		// observe the bumped generation and a freshly-arrived handshake could flip bHandshakeOk=true
		// (HandleHandshake sets it under this SAME lock) before the delayed bHandshakeOk=false landed —
		// clobbering a just-authenticated connection back to pre-handshake until the 10 s deadline recycled it.
		bHandshakeOk = false;
		bClientConnected = true;
		bHeartbeatStop = true; // retire any heartbeat tied to the prior connection; a fresh one starts on handshake
	}

	LastActivitySeconds.Set(static_cast<int32>(FPlatformTime::Seconds()));
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] sidecar connected from %s; awaiting handshake."), *Endpoint.ToString());
	return true; // keep the socket — we own it now
}

bool FUnrealMcpBridgeServer::Init() { return true; }

uint32 FUnrealMcpBridgeServer::Run()
{
	// Pre-handshake connections must not hold the single slot indefinitely (a silent dialer that never
	// sends the handshake). Drop them after this deadline (§6 — the heartbeat only guards POST-handshake
	// silence). 10 s comfortably covers the sidecar's own 10 s ack timeout on the other side.
	constexpr double PreHandshakeDeadlineSeconds = 10.0;

	FUnrealMcpNdjsonAccumulator Accumulator;
	int32 ServedGeneration = -1;
	uint8 Buffer[64 * 1024];

	while (!bStopRequested)
	{
		// Only the reader frees sockets (see SocketsPendingDestroy) — do it at a point where we are not
		// inside a Recv on any of them.
		DrainPendingDestroy();

		FSocket* Sock;
		int32 CurrentGeneration;
		double AcceptedAt;
		{
			FScopeLock Lock(&ConnectionMutex);
			Sock = ClientSocket;
			CurrentGeneration = ConnectionGeneration;
			AcceptedAt = ConnectionAcceptedSeconds;
		}

		// A new connection (or a replacement) resets the framer so a partial line from the OLD socket can
		// never prepend the new handshake (the stale-accumulator corruption).
		if (CurrentGeneration != ServedGeneration)
		{
			Accumulator = FUnrealMcpNdjsonAccumulator();
			ServedGeneration = CurrentGeneration;
		}

		if (Sock == nullptr)
		{
			FPlatformProcess::Sleep(0.05f);
			continue;
		}

		if (!bHandshakeOk && (FPlatformTime::Seconds() - AcceptedAt) > PreHandshakeDeadlineSeconds)
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] connection sent no valid handshake within %.0fs; dropping."), PreHandshakeDeadlineSeconds);
			CloseActiveConnection();
			continue;
		}

		if (!Sock->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200)))
			continue;

		int32 BytesRead = 0;
		const bool bRecvOk = Sock->Recv(Buffer, sizeof(Buffer), BytesRead);

		// The socket may have been superseded by a new accept while we were in Wait/Recv. If so, discard
		// what we read: it belongs to a connection that is already being torn down (and is about to be
		// freed by the next DrainPendingDestroy()).
		{
			FScopeLock Lock(&ConnectionMutex);
			if (ConnectionGeneration != CurrentGeneration)
				continue;
		}

		if (!bRecvOk || BytesRead == 0)
		{
			// Peer closed or errored — drop the connection and wait for a re-dial.
			CloseActiveConnection();
			continue;
		}

		LastActivitySeconds.Set(static_cast<int32>(FPlatformTime::Seconds()));

		TArray<FString> Lines;
		if (!Accumulator.Push(Buffer, BytesRead, Lines))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] IPC line exceeded the frame cap; dropping connection."));
			CloseActiveConnection();
			continue;
		}

		for (const FString& Line : Lines)
		{
			if (!Line.IsEmpty())
				HandleLine(Line, CurrentGeneration);
		}
	}
	return 0;
}

void FUnrealMcpBridgeServer::Stop()
{
	bStopRequested = true;
}

void FUnrealMcpBridgeServer::Exit() {}

void FUnrealMcpBridgeServer::HandleLine(const FString& Line, int32 Generation)
{
	TSharedPtr<FJsonObject> Message;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] dropped malformed IPC line."));
		return;
	}

	FString Type;
	if (!Message->TryGetStringField(TEXT("type"), Type))
		return;

	// §1.4 auth gate: until the handshake is validated with the stdin token, the ONLY message we honour is
	// the handshake itself. Drop every other type (tool-call/tool-cancel/ping/...) — a local process that
	// guessed the deterministic loopback port must never invoke tools or drive the bridge without the token.
	if (!bHandshakeOk && Type != TypeHandshake)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] dropping pre-handshake IPC message of type '%s'."), *Type);
		return;
	}

	if (Type == TypeHandshake)        HandleHandshake(Message, Generation);
	else if (Type == TypeToolCall)    HandleToolCall(Message);
	else if (Type == TypeToolCancel)  HandleToolCancel(Message);
	else if (Type == TypePing)
	{
		TSharedPtr<FJsonObject> Pong = MakeShared<FJsonObject>();
		Pong->SetStringField(TEXT("type"), TypePong);
		SendMessage(Pong);
	}
	else if (Type == TypePong)
	{
		// liveness already refreshed in Run()
	}
	else if (Type == TypeStatus || Type == TypeDeviceAuth || Type == TypeAgentConfigResult)
	{
		// §1.3 / §7: the live UI feed (connection status, device-auth, AND the AI-agent configurator results).
		// Hand it to the registered sink (the view-model) WITHOUT touching any Slate/engine state here — the
		// sink marshals onto the game thread (M9b) and routes by type. Copy the sink out under the lock so a
		// concurrent window-close clear never invalidates the TFunction mid-call.
		TFunction<void(const FString&, TSharedPtr<FJsonObject>)> SinkCopy;
		{
			FScopeLock Lock(&StatusSinkMutex);
			SinkCopy = StatusSink;
		}
		if (SinkCopy)
			SinkCopy(Type, Message);
	}
	else
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] ignoring IPC message of type '%s'."), *Type);
	}
}

void FUnrealMcpBridgeServer::HandleHandshake(const TSharedPtr<FJsonObject>& Message, int32 Generation)
{
	FString IncomingToken;
	Message->TryGetStringField(TEXT("token"), IncomingToken);

	// Case-SENSITIVE compare: the token is a hex secret, so a case-folding match (FString operator==)
	// would needlessly widen the accepted set.
	if (Token.IsEmpty() || !IncomingToken.Equals(Token, ESearchCase::CaseSensitive))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] handshake rejected (token mismatch); closing connection."));
		CloseActiveConnection();
		return;
	}

	// Close the check-to-flip race (§1.4): the accept thread can swap in a NEW connection — bumping the
	// generation and parking the old socket — between the reader's post-Recv generation check and here. If
	// this handshake line came off a now-superseded socket, flipping bHandshakeOk and sending the ack +
	// manifest would authenticate (and write to) the CURRENT, still-unauthenticated connection. Re-verify
	// the generation we read this batch at, under the connection lock, and bail if it has moved on.
	{
		FScopeLock Lock(&ConnectionMutex);
		if (ConnectionGeneration != Generation)
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] handshake arrived for a superseded connection (gen %d != %d); ignoring."),
				Generation, ConnectionGeneration);
			return;
		}
		bHandshakeOk = true;
	}

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] handshake accepted."));
	SendHandshakeAck();

	// §1.5: on Ready immediately push the manifest AND the effective connection config (§1.3 `config`, §8).
	// The handshake-ack already carries the config for the sidecar's initial SignalR connect; the standalone
	// `config` message satisfies the §8 "on Ready and on change" contract and is the channel a later UI edit
	// re-pushes through (SetEffectiveConfig → PushConfig).
	{
		FScopeLock Lock(&WriteMutex);
		SendManifestLocked();
		SendConfigLocked();
	}

	StartHeartbeat();

	// Issue #99: notify the §7 view-model (via the game-thread-marshalling sink) that the sidecar is now connected
	// + handshaken, so a Cloud auth-start queued while the bridge was (re)starting can be flushed exactly here. Copy
	// the sink under the lock and invoke OUTSIDE it (the sink marshals to the game thread; never hold a lock across).
	TFunction<void()> HandshakeSinkCopy;
	{
		FScopeLock Lock(&StatusSinkMutex);
		HandshakeSinkCopy = HandshakeSink;
	}
	if (HandshakeSinkCopy)
		HandshakeSinkCopy();
}

void FUnrealMcpBridgeServer::HandleToolCall(const TSharedPtr<FJsonObject>& Message)
{
	// Do not start new work once teardown has begun — the continuation captures `this` and the body the
	// registry, both of which the runtime frees right after Shutdown() (see DrainInFlightCalls).
	if (bStopRequested)
		return;

	FString RequestId, ToolName;
	Message->TryGetStringField(TEXT("requestId"), RequestId);
	Message->TryGetStringField(TEXT("tool"), ToolName);

	int32 TimeoutMs = DefaultToolTimeoutMs;
	double TimeoutValue;
	if (Message->TryGetNumberField(TEXT("timeoutMs"), TimeoutValue) && TimeoutValue > 0)
		TimeoutMs = static_cast<int32>(TimeoutValue);

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ArgsPtr;
	if (Message->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr->IsValid())
		Arguments = *ArgsPtr;

	// Per-call cancel flag (§4). Shared ownership keeps it alive for the lifetime of the dispatched call
	// even after we drop the map entry below — the dispatcher's captured copy of FUnrealMcpToolCall holds
	// a reference, so a late IsCancelled() can never deref freed memory.
	TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);

	// Never trust the sidecar's requestId for map uniqueness: an empty or duplicate id would collide and
	// silently drop a prior in-flight call's flag. Only correlate cancellation for a non-empty, unique id.
	bool bTrackCancel = false;
	if (!RequestId.IsEmpty())
	{
		FScopeLock Lock(&CancelMutex);
		if (CancelFlags.Contains(RequestId))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] duplicate tool-call requestId '%s'; not tracking cancellation for it."), *RequestId);
		}
		else
		{
			CancelFlags.Add(RequestId, CancelFlag);
			bTrackCancel = true;
		}
	}

	FUnrealMcpToolCall Call(Arguments);
	Call.CancelRequested = CancelFlag;

	const FString CapturedTool = ToolName;
	const FString CapturedRequestId = RequestId;
	const bool bRemoveOnDone = bTrackCancel;

	InFlightCalls.Increment();

	Dispatcher.Dispatch(
		Call,
		[this, CapturedTool](const FUnrealMcpToolCall& C) { return Registry.Execute(CapturedTool, C); },
		FTimespan::FromMilliseconds(TimeoutMs))
		.Next([this, CapturedRequestId, bRemoveOnDone](FUnrealMcpToolResult Result)
		{
			TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("type"), TypeToolResponse);
			Response->SetStringField(TEXT("requestId"), CapturedRequestId);
			Response->SetStringField(TEXT("status"), Result.bSuccess ? TEXT("success") : TEXT("error"));

			TArray<TSharedPtr<FJsonValue>> Content;
			if (!Result.Message.IsEmpty())
			{
				TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
				TextBlock->SetStringField(TEXT("type"), TEXT("text"));
				TextBlock->SetStringField(TEXT("text"), Result.Message);
				TextBlock->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
				Content.Add(MakeShared<FJsonValueObject>(TextBlock));
			}
			// Image content blocks (§1.3 MCP content array) — e.g. the §10 screenshot family. The
			// sidecar maps each onto an MCP image ContentBlock (ProxyResponseMapper) with no re-shaping.
			for (const FUnrealMcpImageContent& Image : Result.Images)
			{
				TSharedPtr<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
				ImageBlock->SetStringField(TEXT("type"), TEXT("image"));
				ImageBlock->SetStringField(TEXT("data"), Image.Base64Data);
				ImageBlock->SetStringField(TEXT("mimeType"), Image.MimeType);
				Content.Add(MakeShared<FJsonValueObject>(ImageBlock));
			}
			Response->SetArrayField(TEXT("content"), Content);

			if (Result.Structured.IsValid())
				Response->SetObjectField(TEXT("structured"), Result.Structured);

			SendMessage(Response);

			if (bRemoveOnDone)
			{
				FScopeLock Lock(&CancelMutex);
				CancelFlags.Remove(CapturedRequestId);
			}
			InFlightCalls.Decrement();
		});
}

void FUnrealMcpBridgeServer::HandleToolCancel(const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	if (!Message->TryGetStringField(TEXT("requestId"), RequestId))
		return;

	FScopeLock Lock(&CancelMutex);
	if (TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe>* Flag = CancelFlags.Find(RequestId))
		(*Flag).Get() = true;
}

bool FUnrealMcpBridgeServer::SendMessage(const TSharedPtr<FJsonObject>& Message)
{
	const FString Json = SerializeCondensed(Message);
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(Json);

	FScopeLock WriteLock(&WriteMutex);
	FScopeLock ConnLock(&ConnectionMutex);
	if (ClientSocket == nullptr)
		return false;

	if (!TrySendFramedLocked(Framed))
	{
		// A genuine (non-would-block) failure mid-frame corrupts the stream — the peer would see a truncated
		// line with the next frame concatenated, and a lost tool-response hangs the pending call until the
		// heartbeat. Drop the connection so the sidecar reconnects cleanly. We hold ConnectionMutex, so just
		// park the socket for the reader to free (never DestroySocket from here) and clear flags.
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] socket send failed mid-frame; dropping connection."));
		SocketsPendingDestroy.Add(ClientSocket);
		ClientSocket = nullptr;
		bClientConnected = false;
		bHandshakeOk = false;
		bHeartbeatStop = true;
		return false;
	}
	return true;
}

bool FUnrealMcpBridgeServer::TrySendFramedLocked(const TArray<uint8>& Framed)
{
	// Caller holds WriteMutex AND ConnectionMutex and has verified ClientSocket != nullptr. Sends the whole
	// frame, tolerating EWOULDBLOCK on the non-blocking client socket: a full kernel send buffer (a large
	// frame on a slow reader) makes FSocket::Send return false with SE_EWOULDBLOCK — that is transient, not
	// fatal, so we wait (bounded) for writability and retry the same offset rather than dropping a healthy
	// connection mid-frame. Returns false only on a genuine send error or the per-frame deadline.
	// We deliberately keep ConnectionMutex held across the bounded wait so the reader cannot free the socket
	// under us (the pass-1 second-connection use-after-free); the wait is bounded, so accepts are only
	// briefly stalled — and only on the (currently unreachable, all outbound frames are tiny) large-frame
	// path. Releasing the lock during the wait is the connection-replacement/socket-lifetime redesign that
	// is tracked separately.
	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	const double Deadline = FPlatformTime::Seconds() + 2.0;

	int32 TotalSent = 0;
	while (TotalSent < Framed.Num())
	{
		int32 JustSent = 0;
		const bool bSendOk = ClientSocket->Send(Framed.GetData() + TotalSent, Framed.Num() - TotalSent, JustSent);
		if (JustSent > 0)
		{
			TotalSent += JustSent;
			continue;
		}

		// No bytes moved: distinguish a transient full send buffer (EWOULDBLOCK / EAGAIN) from a real error.
		const ESocketErrors Err = SocketSub != nullptr ? SocketSub->GetLastErrorCode() : SE_NO_ERROR;
		const bool bWouldBlock = bSendOk || Err == SE_EWOULDBLOCK || Err == SE_TRY_AGAIN;
		if (!bWouldBlock || FPlatformTime::Seconds() >= Deadline)
			return false;

		// Wait (bounded) for the socket to become writable, then retry from the same offset.
		ClientSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(100));
	}
	return true;
}

void FUnrealMcpBridgeServer::SendHandshakeAck()
{
	TSharedPtr<FJsonObject> Ack = MakeShared<FJsonObject>();
	Ack->SetStringField(TEXT("type"), TypeHandshakeAck);
	Ack->SetNumberField(TEXT("ipcVersion"), IpcVersion);
	Ack->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Ack->SetStringField(TEXT("engineVersion"), EngineVersion);
	Ack->SetStringField(TEXT("projectPath"), ProjectPath);

	// §1.3/§1.5: the ack carries the effective connection config so the sidecar's FIRST SignalR connect uses
	// the plugin-resolved mode/host/cloudUrl/token (the sidecar never re-resolves §8). Tokens are part of the
	// payload but are NEVER logged here (§8) — only the message shape is.
	{
		FScopeLock Lock(&ConfigMutex);
		if (EffectiveConfig.IsValid())
			Ack->SetObjectField(TEXT("config"), EffectiveConfig);
	}

	SendMessage(Ack);
}

void FUnrealMcpBridgeServer::SendManifestLocked()
{
	// Caller holds WriteMutex. Reading the registry here from the IPC reader thread is safe ONLY because
	// the registry is mutated exclusively at startup (UnrealMcpPingTool::Register runs before the bridge
	// accepts), so the manifest is stable by the time any handshake arrives. Dynamic re-registration (the
	// §2.2 hot-reload path) MUST instead marshal the manifest build through the game-thread dispatcher.
	const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
	const FString Json = SerializeCondensed(Manifest);
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(Json);

	FScopeLock ConnLock(&ConnectionMutex);
	if (ClientSocket == nullptr)
		return;

	if (!TrySendFramedLocked(Framed))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] manifest send failed mid-frame; dropping connection."));
		SocketsPendingDestroy.Add(ClientSocket);
		ClientSocket = nullptr;
		bClientConnected = false;
		bHandshakeOk = false;
		bHeartbeatStop = true;
		return;
	}
}

void FUnrealMcpBridgeServer::PushManifest()
{
	if (!bClientConnected || !bHandshakeOk)
		return;
	FScopeLock Lock(&WriteMutex);
	SendManifestLocked();
}

void FUnrealMcpBridgeServer::SendConfigLocked()
{
	// Caller holds WriteMutex. Build a fresh `config` envelope wrapping the effective §8 connection config.
	TSharedPtr<FJsonObject> ConfigCopy;
	{
		FScopeLock Lock(&ConfigMutex);
		ConfigCopy = EffectiveConfig;
	}
	if (!ConfigCopy.IsValid())
		return; // no config to push (the sidecar keeps its env fallback)

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TypeConfig);
	// Spread the effective config fields (mode/host/cloudUrl/token/keepConnected) onto the envelope so the
	// §1.3 `config` message is flat (the sidecar reads them off the top level, like the handshake-ack does).
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : ConfigCopy->Values)
		Message->SetField(Field.Key, Field.Value);

	const FString Json = SerializeCondensed(Message);
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(Json);

	FScopeLock ConnLock(&ConnectionMutex);
	if (ClientSocket == nullptr)
		return;

	if (!TrySendFramedLocked(Framed))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] config send failed mid-frame; dropping connection."));
		SocketsPendingDestroy.Add(ClientSocket);
		ClientSocket = nullptr;
		bClientConnected = false;
		bHandshakeOk = false;
		bHeartbeatStop = true;
	}
}

void FUnrealMcpBridgeServer::PushConfig()
{
	if (!bClientConnected || !bHandshakeOk)
		return;
	FScopeLock Lock(&WriteMutex);
	SendConfigLocked();
}

void FUnrealMcpBridgeServer::SetEffectiveConfig(const TSharedPtr<FJsonObject>& InEffectiveConfig)
{
	{
		FScopeLock Lock(&ConfigMutex);
		EffectiveConfig = CloneJsonObject(InEffectiveConfig);
	}
	PushConfig(); // "on change" (§8) — no-op when no sidecar is connected
}

bool FUnrealMcpBridgeServer::SendAuthMessage(const FString& AuthType)
{
	// Only the three §1.3 auth verbs are valid here; reject anything else so a UI bug cannot smuggle an
	// arbitrary message type onto the wire.
	if (AuthType != TypeAuthStart && AuthType != TypeAuthCancel && AuthType != TypeAuthRevoke)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] refusing to send unknown auth message type '%s'."), *AuthType);
		return false;
	}
	if (!bClientConnected || !bHandshakeOk)
		return false;

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), AuthType);
	return SendMessage(Message);
}

bool FUnrealMcpBridgeServer::SendAgentConfigMessage(const TSharedPtr<FJsonObject>& Message)
{
	if (!Message.IsValid())
		return false;

	// Only the §7 agent-config request verbs are valid here, so a UI bug cannot smuggle an arbitrary message
	// type onto the wire (mirrors SendAuthMessage's allow-list).
	FString Type;
	if (!Message->TryGetStringField(TEXT("type"), Type) ||
		(Type != TypeAgentsList && Type != TypeAgentStatus && Type != TypeAgentConfigure &&
		 Type != TypeAgentRemove && Type != TypeAgentSkillsPath))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] refusing to send unknown agent-config message type '%s'."), *Type);
		return false;
	}
	if (!bClientConnected || !bHandshakeOk)
		return false;

	return SendMessage(Message);
}

void FUnrealMcpBridgeServer::SetStatusSink(TFunction<void(const FString&, TSharedPtr<FJsonObject>)> InSink)
{
	FScopeLock Lock(&StatusSinkMutex);
	StatusSink = MoveTemp(InSink);
}

void FUnrealMcpBridgeServer::SetHandshakeSink(TFunction<void()> InSink)
{
	FScopeLock Lock(&StatusSinkMutex);
	HandshakeSink = MoveTemp(InSink);
}

void FUnrealMcpBridgeServer::CloseActiveConnection()
{
	// Signal the heartbeat to stop but NEVER join it here: this method is itself called on the heartbeat
	// thread (the silent-peer drop path), and joining our own future would self-deadlock — which would
	// wedge the heartbeat thread permanently and hang the editor on quit. The join happens only in
	// StartHeartbeat()/Shutdown(), both off the heartbeat thread (and guarded against self-join anyway).
	bHeartbeatStop = true;

	FScopeLock Lock(&ConnectionMutex);
	if (ClientSocket != nullptr)
	{
		// Defer the actual Close+DestroySocket to the reader thread (DrainPendingDestroy): the reader may
		// be mid-Recv on this very socket on another thread, and freeing it here would be a use-after-free.
		SocketsPendingDestroy.Add(ClientSocket);
		ClientSocket = nullptr;
	}
	bClientConnected = false;
	bHandshakeOk = false;
}

void FUnrealMcpBridgeServer::DrainPendingDestroy()
{
	TArray<FSocket*> ToDestroy;
	{
		FScopeLock Lock(&ConnectionMutex);
		if (SocketsPendingDestroy.Num() == 0)
			return;
		ToDestroy = MoveTemp(SocketsPendingDestroy);
		SocketsPendingDestroy.Reset();
	}

	// Only ever reached on the reader thread (per loop iteration) or in Shutdown() after the reader is
	// dead — i.e. a single-owner context — so closing+freeing here cannot race a concurrent Recv.
	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	for (FSocket* S : ToDestroy)
	{
		if (S != nullptr)
		{
			S->Close();
			SocketSub->DestroySocket(S);
		}
	}
}

void FUnrealMcpBridgeServer::StartHeartbeat()
{
	// Join any prior heartbeat before launching a fresh one. Safe: StartHeartbeat runs on the reader
	// thread (via HandleHandshake), never on the heartbeat thread, so the join cannot self-deadlock.
	StopHeartbeat();

	bHeartbeatStop = false;
	HeartbeatFuture = Async(EAsyncExecution::Thread, [this]()
	{
		HeartbeatThreadId.store(FPlatformTLS::GetCurrentThreadId());
		double LastPingSeconds = FPlatformTime::Seconds();

		// Poll on a short tick (not a full interval sleep) so StopHeartbeat()/connection replacement can
		// retire this thread promptly instead of blocking a join for up to a whole heartbeat interval.
		while (!bHeartbeatStop && !bStopRequested)
		{
			FPlatformProcess::Sleep(0.5f);
			if (bHeartbeatStop || bStopRequested)
				break;
			if (!bClientConnected || !bHandshakeOk)
				continue;

			const int32 Now = static_cast<int32>(FPlatformTime::Seconds());
			if (Now - LastActivitySeconds.GetValue() > HeartbeatTimeoutSeconds)
			{
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] sidecar silent > %ds; dropping connection."), HeartbeatTimeoutSeconds);
				CloseActiveConnection(); // sets bHeartbeatStop (no self-join) and parks the socket for the reader
				break;
			}

			if (FPlatformTime::Seconds() - LastPingSeconds >= static_cast<double>(HeartbeatIntervalSeconds))
			{
				LastPingSeconds = FPlatformTime::Seconds();
				TSharedPtr<FJsonObject> Ping = MakeShared<FJsonObject>();
				Ping->SetStringField(TEXT("type"), TypePing);
				SendMessage(Ping);
			}
		}
		HeartbeatThreadId.store(0);
	});
}

void FUnrealMcpBridgeServer::StopHeartbeat()
{
	bHeartbeatStop = true;

	// Defence in depth against the self-deadlock: never block-join from the heartbeat thread itself.
	if (HeartbeatThreadId.load() == FPlatformTLS::GetCurrentThreadId())
		return;

	if (HeartbeatFuture.IsValid())
	{
		HeartbeatFuture.Wait();
		HeartbeatFuture = TFuture<void>();
	}
}

void FUnrealMcpBridgeServer::Shutdown()
{
	if (bStopRequested && Listener == nullptr && ReaderThread == nullptr)
		return;

	bStopRequested = true;

	// Ask the sidecar to exit gracefully (§1.5).
	if (bClientConnected && bHandshakeOk)
	{
		TSharedPtr<FJsonObject> Bye = MakeShared<FJsonObject>();
		Bye->SetStringField(TEXT("type"), TypeShutdown);
		SendMessage(Bye);
	}

	StopHeartbeat();

	// Stop accepting (no more connection swaps) and stop the reader (no more socket Recv/destroy) BEFORE we
	// free any client socket — after this, this thread is the sole owner of the connection state.
	if (Listener != nullptr)
	{
		delete Listener;
		Listener = nullptr;
	}

	if (ReaderThread != nullptr)
	{
		ReaderThread->Kill(true);
		delete ReaderThread;
		ReaderThread = nullptr;
	}

	// Drain in-flight dispatched calls before returning (the runtime frees this server, the dispatcher and
	// the registry right after Shutdown()): each .Next continuation captures `this` and each body captures
	// the registry, so a call still in flight would fire on freed objects. Cancel cooperatively, then wait
	// a bounded grace for the in-flight counter to reach zero. (The only core tool today is `ping`, which
	// drains in microseconds; the bound guarantees we never hang the editor on a misbehaving tool.)
	CancelAllInFlight();
	DrainInFlightCalls(FTimespan::FromSeconds(5));

	CloseActiveConnection();
	DrainPendingDestroy(); // reader is dead — safe to free here

	if (ListenSocket != nullptr)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}
}

void FUnrealMcpBridgeServer::CancelAllInFlight()
{
	FScopeLock Lock(&CancelMutex);
	for (TPair<FString, TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe>>& Pair : CancelFlags)
		Pair.Value.Get() = true;
}

void FUnrealMcpBridgeServer::DrainInFlightCalls(FTimespan Grace)
{
	const double Deadline = FPlatformTime::Seconds() + Grace.GetTotalSeconds();
	while (InFlightCalls.GetValue() > 0 && FPlatformTime::Seconds() < Deadline)
		FPlatformProcess::Sleep(0.02f);

	if (InFlightCalls.GetValue() > 0)
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %d tool call(s) still in flight at shutdown after grace; proceeding."), InFlightCalls.GetValue());
}
