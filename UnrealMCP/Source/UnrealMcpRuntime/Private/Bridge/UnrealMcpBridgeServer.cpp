// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpBridgeServer.h"
#include "UnrealMcpNdjson.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpPromptRegistry.h"
#include "UnrealMcpResourceRegistry.h"
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
	// v2 prompt/resource message families (docs/ARCHITECTURE.md §A.1) — exchanged only on a v2-negotiated link.
	const FString TypePromptManifest = TEXT("prompt-manifest");
	const FString TypeResourceManifest = TEXT("resource-manifest");
	const FString TypePromptGet = TEXT("prompt-get");
	const FString TypeResourceRead = TEXT("resource-read");
	const FString TypePromptResponse = TEXT("prompt-response");
	const FString TypeResourceResponse = TEXT("resource-response");
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
	const FString TypeAgentGenerateSkills = TEXT("agent-generate-skills");
	const FString TypeAgentConfigResult = TEXT("agent-config-result");
	// mcp-authorize PR 4 IPC verbs (design 04/06): the plugin → sidecar `project-config` request and the
	// sidecar → plugin `project-config-result`. The sidecar resolves THIS project's {pin, derived local-server
	// port, serverTarget} via the shared C# ProjectIdentity + project marker (no C++ derivation duplication).
	const FString TypeProjectConfig = TEXT("project-config");
	const FString TypeProjectConfigResult = TEXT("project-config-result");
	// mcp-authorize g5/g6 consolidation: the plugin → sidecar `server-launch-args` request and the sidecar →
	// plugin `server-launch-args-result`. The sidecar composes the local-server launch-arg string via the SHARED
	// ServerLaunchArguments builder (none/oauth/token) so the C++ side holds no duplicate arg logic.
	const FString TypeServerLaunchArgs = TEXT("server-launch-args");
	const FString TypeServerLaunchArgsResult = TEXT("server-launch-args-result");
	const FString TypePing = TEXT("ping");
	const FString TypePong = TEXT("pong");
	const FString TypeShutdown = TEXT("shutdown");

	// IPC wire-protocol version (§9.2 / §A.1). v2 adds the prompt/resource message families; it is NEGOTIATED on
	// the handshake (min of this plugin's version and the sidecar's advertised ipcVersion). A v1 sidecar paired
	// with this v2 plugin negotiates down to 1 and the link stays tools-only — the tool path is identical at every
	// version, so a version mismatch never breaks tools (an old sidecar keeps working).
	constexpr int32 IpcVersion = 2;
	// The lowest negotiated version at which the prompt/resource families are exchanged (§A.1).
	constexpr int32 PromptsResourcesMinVersion = 2;
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

FUnrealMcpBridgeServer::FUnrealMcpBridgeServer(FUnrealMcpToolRegistry& InRegistry, FUnrealMcpGameThreadDispatcher& InDispatcher,
	FUnrealMcpPromptRegistry* InPromptRegistry, FUnrealMcpResourceRegistry* InResourceRegistry)
	: Registry(InRegistry)
	, PromptRegistry(InPromptRegistry)
	, ResourceRegistry(InResourceRegistry)
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

int32 FUnrealMcpBridgeServer::NegotiateIpcVersion(int32 LocalVersion, int32 RemoteVersion)
{
	// min(local, remote) = the highest version both peers understand (§A.1). A non-positive / absent remote
	// (a legacy handshake that omitted the field) is treated as v1. Mirrors the sidecar's
	// IpcProtocol.NegotiateVersion so both ends agree on the negotiated version without re-exchanging it.
	const int32 Remote = RemoteVersion > 0 ? RemoteVersion : 1;
	const int32 Local = LocalVersion > 0 ? LocalVersion : 1;
	return FMath::Min(Local, Remote);
}

int32 FUnrealMcpBridgeServer::GetIpcVersion()
{
	return IpcVersion;
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
	else if (Type == TypePromptGet)   HandlePromptGet(Message);     // v2 scaffold stub (§A.1)
	else if (Type == TypeResourceRead) HandleResourceRead(Message); // v2 scaffold stub (§A.1)
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
	else if (Type == TypeStatus || Type == TypeDeviceAuth || Type == TypeAgentConfigResult || Type == TypeProjectConfigResult || Type == TypeServerLaunchArgsResult)
	{
		// §1.3 / §7 / mcp-authorize PR 4 + g5/g6: the live UI + control feed (connection status, device-auth, the
		// AI-agent configurator results, the resolved project-config result carrying the derived local-server port,
		// AND the composed server-launch-args result the local-server start awaits).
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
		// §A.1 version negotiation: the link runs at min(this plugin, sidecar). A v1 sidecar negotiates down to
		// 1 (tools-only); a v2 sidecar enables the prompt/resource families. Read the sidecar's advertised version
		// off the handshake (absent/legacy → v1). Stored under ConnectionMutex alongside bHandshakeOk so the
		// post-handshake manifest push (same reader thread) reads a settled value.
		int32 SidecarIpcVersion = 0;
		{
			double VersionNumber = 0.0;
			if (Message->TryGetNumberField(TEXT("ipcVersion"), VersionNumber))
				SidecarIpcVersion = static_cast<int32>(VersionNumber);
		}
		NegotiatedIpcVersion = FUnrealMcpBridgeServer::NegotiateIpcVersion(IpcVersion, SidecarIpcVersion);
		bHandshakeOk = true;
	}

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] handshake accepted (IPC v%d negotiated; prompts/resources %s)."),
		NegotiatedIpcVersion,
		NegotiatedIpcVersion >= PromptsResourcesMinVersion ? TEXT("ENABLED") : TEXT("tools-only"));
	SendHandshakeAck();

	// §1.5: on Ready immediately push the manifest(s) AND the effective connection config (§1.3 `config`, §8).
	// The handshake-ack already carries the config for the sidecar's initial SignalR connect; the standalone
	// `config` message satisfies the §8 "on Ready and on change" contract and is the channel a later UI edit
	// re-pushes through (SetEffectiveConfig → PushConfig). The prompt/resource manifests are §A.1 scaffold
	// no-ops until P1/P2 wire the registries (and are gated on a v2-negotiated link), but pushing them here
	// keeps the on-Ready ordering identical to what P1/P2 expect.
	{
		FScopeLock Lock(&WriteMutex);
		SendManifestLocked();
		SendPromptManifestLocked();
		SendResourceManifestLocked();
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

void FUnrealMcpBridgeServer::HandlePromptGet(const TSharedPtr<FJsonObject>& Message)
{
	// Do not start new work once teardown has begun (same contract as HandleToolCall): the continuation
	// captures `this` and the body the prompt registry, both freed right after Shutdown().
	if (bStopRequested)
		return;

	FString RequestId, PromptName;
	Message->TryGetStringField(TEXT("requestId"), RequestId);
	Message->TryGetStringField(TEXT("prompt"), PromptName);

	// Defensive: a prompt-get must never arrive on a tools-only build (the v2 gate blocks the manifest), but
	// answer with a correlated error rather than dropping the pending call if PromptRegistry is null.
	if (PromptRegistry == nullptr)
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("type"), TypePromptResponse);
		Response->SetStringField(TEXT("requestId"), RequestId);
		Response->SetStringField(TEXT("status"), TEXT("error"));
		Response->SetStringField(TEXT("error"), TEXT("Prompts are not implemented."));
		SendMessage(Response);
		return;
	}

	int32 TimeoutMs = DefaultToolTimeoutMs;
	double TimeoutValue;
	if (Message->TryGetNumberField(TEXT("timeoutMs"), TimeoutValue) && TimeoutValue > 0)
		TimeoutMs = static_cast<int32>(TimeoutValue);

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ArgsPtr;
	if (Message->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr->IsValid())
		Arguments = *ArgsPtr;

	// Per-call cancel flag (§4) — reuse the SAME CancelFlags map as tool-calls (the generic tool-cancel by
	// requestId covers all kinds, §A.1). Shared ownership keeps it alive past the map-entry removal.
	TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);

	bool bTrackCancel = false;
	if (!RequestId.IsEmpty())
	{
		FScopeLock Lock(&CancelMutex);
		if (CancelFlags.Contains(RequestId))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] duplicate prompt-get requestId '%s'; not tracking cancellation for it."), *RequestId);
		}
		else
		{
			CancelFlags.Add(RequestId, CancelFlag);
			bTrackCancel = true;
		}
	}

	FUnrealMcpToolCall Call(Arguments);
	Call.CancelRequested = CancelFlag;

	const FString CapturedPrompt = PromptName;
	const FString CapturedRequestId = RequestId;
	const bool bRemoveOnDone = bTrackCancel;
	FUnrealMcpPromptRegistry* CapturedRegistry = PromptRegistry;

	InFlightCalls.Increment();

	// The dispatcher's Dispatch is hard-typed to FUnrealMcpToolResult, so marshal the prompt Execute onto the
	// game thread through it and PACK the FUnrealMcpPromptResult into a FUnrealMcpToolResult transport: the
	// Structured JSON carries { description, messages:[{role,text}] } and the bSuccess flag rides on
	// FUnrealMcpToolResult::bSuccess / Message (the error reason). The .Next unpacks it into a prompt-response.
	// This keeps the game-thread + per-call timeout + cancel semantics identical to the tool path with the
	// least new surface (no second Dispatch overload / no parallel registry plumbing in the dispatcher).
	Dispatcher.Dispatch(
		Call,
		[CapturedRegistry, CapturedPrompt](const FUnrealMcpToolCall& C) -> FUnrealMcpToolResult
		{
			const FUnrealMcpPromptResult PromptResult = CapturedRegistry->Execute(CapturedPrompt, C);

			TSharedPtr<FJsonObject> Packed = MakeShared<FJsonObject>();
			Packed->SetStringField(TEXT("description"), PromptResult.Description);
			TArray<TSharedPtr<FJsonValue>> Messages;
			for (const FUnrealMcpPromptMessage& Msg : PromptResult.Messages)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("role"), Msg.Role == EUnrealMcpPromptRole::Assistant ? TEXT("assistant") : TEXT("user"));
				Entry->SetStringField(TEXT("text"), Msg.Text);
				Messages.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Packed->SetArrayField(TEXT("messages"), Messages);

			// On success Message is unused; on error it carries the reason so the .Next surfaces it as `error`.
			FUnrealMcpToolResult Transport = PromptResult.bSuccess
				? FUnrealMcpToolResult::Success(FString(), Packed)
				: FUnrealMcpToolResult::Error(PromptResult.Description);
			Transport.Structured = Packed; // Error() drops Structured — re-attach so the .Next can read messages
			return Transport;
		},
		FTimespan::FromMilliseconds(TimeoutMs))
		.Next([this, CapturedRequestId, bRemoveOnDone](FUnrealMcpToolResult Result)
		{
			TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("type"), TypePromptResponse);
			Response->SetStringField(TEXT("requestId"), CapturedRequestId);
			Response->SetStringField(TEXT("status"), Result.bSuccess ? TEXT("success") : TEXT("error"));

			if (Result.bSuccess)
			{
				// Unpack the transport: description + messages[] (the timeout path leaves Structured null —
				// then this is an empty success, which the sidecar maps to a no-message prompt response).
				if (Result.Structured.IsValid())
				{
					FString Description;
					if (Result.Structured->TryGetStringField(TEXT("description"), Description))
						Response->SetStringField(TEXT("description"), Description);

					const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
					if (Result.Structured->TryGetArrayField(TEXT("messages"), Messages))
						Response->SetArrayField(TEXT("messages"), *Messages);
				}
			}
			else
			{
				// Result.Message carries the error reason (registry error, or the dispatcher's timeout error).
				Response->SetStringField(TEXT("error"), Result.Message);
			}

			SendMessage(Response);

			if (bRemoveOnDone)
			{
				FScopeLock Lock(&CancelMutex);
				CancelFlags.Remove(CapturedRequestId);
			}
			InFlightCalls.Decrement();
		});
}

void FUnrealMcpBridgeServer::HandleResourceRead(const TSharedPtr<FJsonObject>& Message)
{
	// Do not start new work once teardown has begun (same contract as HandleToolCall / HandlePromptGet): the
	// continuation captures `this` and the resource registry, both freed right after Shutdown().
	if (bStopRequested)
		return;

	FString RequestId, Uri;
	Message->TryGetStringField(TEXT("requestId"), RequestId);
	Message->TryGetStringField(TEXT("uri"), Uri);

	// Defensive: a resource-read must never arrive on a tools-only build (the v2 gate blocks the manifest), but
	// answer with a correlated error rather than dropping the pending call if ResourceRegistry is null.
	if (ResourceRegistry == nullptr)
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("type"), TypeResourceResponse);
		Response->SetStringField(TEXT("requestId"), RequestId);
		Response->SetStringField(TEXT("status"), TEXT("error"));
		Response->SetStringField(TEXT("error"), TEXT("Resources are not implemented."));
		SendMessage(Response);
		return;
	}

	int32 TimeoutMs = DefaultToolTimeoutMs;
	double TimeoutValue;
	if (Message->TryGetNumberField(TEXT("timeoutMs"), TimeoutValue) && TimeoutValue > 0)
		TimeoutMs = static_cast<int32>(TimeoutValue);

	// Per-call cancel flag (§4) — reuse the SAME CancelFlags map as tool-calls (the generic tool-cancel by
	// requestId covers all kinds, §A.1). Shared ownership keeps it alive past the map-entry removal.
	TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);

	bool bTrackCancel = false;
	if (!RequestId.IsEmpty())
	{
		FScopeLock Lock(&CancelMutex);
		if (CancelFlags.Contains(RequestId))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] duplicate resource-read requestId '%s'; not tracking cancellation for it."), *RequestId);
		}
		else
		{
			CancelFlags.Add(RequestId, CancelFlag);
			bTrackCancel = true;
		}
	}

	// A resource read takes no arguments (static MVP URIs); pass an empty args object + the cancel flag so the
	// dispatcher contract (game thread + per-call timeout + cancel) is identical to the tool/prompt path.
	FUnrealMcpToolCall Call(MakeShared<FJsonObject>());
	Call.CancelRequested = CancelFlag;

	const FString CapturedUri = Uri;
	const FString CapturedRequestId = RequestId;
	const bool bRemoveOnDone = bTrackCancel;
	FUnrealMcpResourceRegistry* CapturedRegistry = ResourceRegistry;

	InFlightCalls.Increment();

	// PACK the FUnrealMcpResourceResult into the FUnrealMcpToolResult transport (same trick as HandlePromptGet):
	// the Structured JSON carries { contents:[{uri,mimeType,text|blob}] }, bSuccess rides on the tool result's
	// flag, and the error reason rides on Message. The .Next unpacks it into a resource-response. This keeps the
	// game-thread + per-call timeout + cancel semantics identical to the tool path with the least new surface.
	Dispatcher.Dispatch(
		Call,
		[CapturedRegistry, CapturedUri](const FUnrealMcpToolCall& /*C*/) -> FUnrealMcpToolResult
		{
			const FUnrealMcpResourceResult ResourceResult = CapturedRegistry->Read(CapturedUri);

			TSharedPtr<FJsonObject> Packed = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Contents;
			for (const FUnrealMcpResourceContent& Content : ResourceResult.Contents)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("uri"), Content.Uri);
				if (!Content.MimeType.IsEmpty())
					Entry->SetStringField(TEXT("mimeType"), Content.MimeType);
				if (Content.bIsBlob)
					Entry->SetStringField(TEXT("blob"), Content.Blob);
				else
					Entry->SetStringField(TEXT("text"), Content.Text);
				Contents.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Packed->SetArrayField(TEXT("contents"), Contents);

			// On success Message is unused; on error it carries the reason so the .Next surfaces it as `error`.
			FUnrealMcpToolResult Transport = ResourceResult.bSuccess
				? FUnrealMcpToolResult::Success(FString(), Packed)
				: FUnrealMcpToolResult::Error(ResourceResult.Error);
			Transport.Structured = Packed; // Error() drops Structured — re-attach so the .Next can read contents
			return Transport;
		},
		FTimespan::FromMilliseconds(TimeoutMs))
		.Next([this, CapturedRequestId, bRemoveOnDone](FUnrealMcpToolResult Result)
		{
			TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("type"), TypeResourceResponse);
			Response->SetStringField(TEXT("requestId"), CapturedRequestId);
			Response->SetStringField(TEXT("status"), Result.bSuccess ? TEXT("success") : TEXT("error"));

			if (Result.bSuccess)
			{
				// Unpack the transport: contents[] (the timeout path leaves Structured null — then this is an
				// empty success, which the sidecar maps to a no-content resource response).
				if (Result.Structured.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* Contents = nullptr;
					if (Result.Structured->TryGetArrayField(TEXT("contents"), Contents))
						Response->SetArrayField(TEXT("contents"), *Contents);
				}
			}
			else
			{
				// Result.Message carries the error reason (registry error, or the dispatcher's timeout error).
				Response->SetStringField(TEXT("error"), Result.Message);
			}

			SendMessage(Response);

			if (bRemoveOnDone)
			{
				FScopeLock Lock(&CancelMutex);
				CancelFlags.Remove(CapturedRequestId);
			}
			InFlightCalls.Decrement();
		});
}

bool FUnrealMcpBridgeServer::SendMessage(const TSharedPtr<FJsonObject>& Message)
{
	const FString Json = SerializeCondensed(Message);
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(Json);

	FScopeLock WriteLock(&WriteMutex);
	return SendFramedLocked(Framed, TEXT("socket send failed mid-frame"));
}

bool FUnrealMcpBridgeServer::SendFramedLocked(const TArray<uint8>& Framed, const TCHAR* Reason)
{
	// Caller holds WriteMutex. Acquire ConnectionMutex for the null-check + send + (on failure) drop, so the
	// reader cannot free the socket under us. A genuine (non-would-block) failure mid-frame corrupts the stream —
	// the peer would see a truncated line with the next frame concatenated, and a lost tool-response hangs the
	// pending call until the heartbeat — so drop the connection for a clean sidecar reconnect.
	FScopeLock ConnLock(&ConnectionMutex);
	if (ClientSocket == nullptr)
		return false;

	if (!TrySendFramedLocked(Framed))
	{
		DropConnectionLocked(Reason);
		return false;
	}
	return true;
}

void FUnrealMcpBridgeServer::DropConnectionLocked(const TCHAR* Reason)
{
	// Caller holds ConnectionMutex. Park the socket for the reader to free (never DestroySocket from a send
	// path — that is DrainPendingDestroy's single-owner job) and clear the connection/handshake/heartbeat flags.
	UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %s; dropping connection."), Reason);
	SocketsPendingDestroy.Add(ClientSocket);
	ClientSocket = nullptr;
	bClientConnected = false;
	bHandshakeOk = false;
	bHeartbeatStop = true;
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
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(SerializeCondensed(Manifest));
	SendFramedLocked(Framed, TEXT("manifest send failed mid-frame"));
}

void FUnrealMcpBridgeServer::PushManifest()
{
	if (!bClientConnected || !bHandshakeOk)
		return;
	FScopeLock Lock(&WriteMutex);
	SendManifestLocked();
}

void FUnrealMcpBridgeServer::PushPromptManifest()
{
	if (!bClientConnected || !bHandshakeOk)
		return;
	FScopeLock Lock(&WriteMutex);
	SendPromptManifestLocked();
}

void FUnrealMcpBridgeServer::PushResourceManifest()
{
	if (!bClientConnected || !bHandshakeOk)
		return;
	FScopeLock Lock(&WriteMutex);
	SendResourceManifestLocked();
}

void FUnrealMcpBridgeServer::SendPromptManifestLocked()
{
	// Caller holds WriteMutex. Gated on a v2-negotiated link — a tools-only (v1) sidecar never receives a
	// prompt-manifest (an old sidecar keeps working). Mirrors SendManifestLocked exactly over the prompt
	// registry's BuildManifestJson. The same startup-stability argument applies (the prompt registry is built
	// before the bridge accepts; dynamic re-registration marshals through the dispatcher).
	if (NegotiatedIpcVersion < PromptsResourcesMinVersion)
		return;
	if (PromptRegistry == nullptr)
		return;

	const TSharedPtr<FJsonObject> Manifest = PromptRegistry->BuildManifestJson();
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(SerializeCondensed(Manifest));
	SendFramedLocked(Framed, TEXT("prompt-manifest send failed mid-frame"));
}

void FUnrealMcpBridgeServer::SendResourceManifestLocked()
{
	// Caller holds WriteMutex. Gated on a v2-negotiated link — a tools-only (v1) sidecar never receives a
	// resource-manifest (an old sidecar keeps working). Mirrors SendPromptManifestLocked exactly over the
	// resource registry's BuildManifestJson. The same startup-stability argument applies (the resource registry
	// is built before the bridge accepts; dynamic re-registration marshals through the dispatcher).
	if (NegotiatedIpcVersion < PromptsResourcesMinVersion)
		return;
	if (ResourceRegistry == nullptr)
		return;

	const TSharedPtr<FJsonObject> Manifest = ResourceRegistry->BuildManifestJson();
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(SerializeCondensed(Manifest));
	SendFramedLocked(Framed, TEXT("resource-manifest send failed mid-frame"));
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

	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(SerializeCondensed(Message));
	SendFramedLocked(Framed, TEXT("config send failed mid-frame"));
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

bool FUnrealMcpBridgeServer::IsValidAgentConfigVerb(const FString& Type)
{
	// Keep in lockstep with the sidecar's IpcProtocol.MessageTypes §7 request verbs — omitting one here
	// silently breaks that UI action (issue #101: a missing `agent-generate-skills` made the Generate button
	// flip the panel to Disconnected because the send was refused).
	return Type == TypeAgentsList || Type == TypeAgentStatus || Type == TypeAgentConfigure ||
		Type == TypeAgentRemove || Type == TypeAgentSkillsPath || Type == TypeAgentGenerateSkills;
}

bool FUnrealMcpBridgeServer::SendAgentConfigMessage(const TSharedPtr<FJsonObject>& Message)
{
	if (!Message.IsValid())
		return false;

	// Only the §7 agent-config request verbs are valid here, so a UI bug cannot smuggle an arbitrary message
	// type onto the wire (mirrors SendAuthMessage's allow-list).
	FString Type;
	if (!Message->TryGetStringField(TEXT("type"), Type) || !IsValidAgentConfigVerb(Type))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] refusing to send unknown agent-config message type '%s'."), *Type);
		return false;
	}
	if (!bClientConnected || !bHandshakeOk)
		return false;

	return SendMessage(Message);
}

bool FUnrealMcpBridgeServer::SendProjectConfigRequest(const FString& RequestId, const FString& InProjectPath)
{
	// mcp-authorize PR 4 (design 04/06): ask the sidecar to resolve THIS project's {pin, derived local-server
	// port, serverTarget}. The result arrives as a `project-config-result` routed through the status sink. No verb
	// allow-list needed (this is not a user-driven UI verb like the auth/agent-config sends) — a single fixed type.
	if (!bClientConnected || !bHandshakeOk)
		return false;

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TypeProjectConfig);
	Message->SetStringField(TEXT("requestId"), RequestId);
	Message->SetStringField(TEXT("projectPath"), InProjectPath);
	return SendMessage(Message);
}

bool FUnrealMcpBridgeServer::SendServerLaunchArgsRequest(
	const FString& RequestId, int32 InPort, int32 InPluginTimeoutMs, const FString& InAuthMode,
	const FString& InToken, const FString& InAuthIssuer, const FString& InPublicUrl)
{
	// mcp-authorize g5/g6 consolidation: ask the sidecar to COMPOSE the local-server launch-arg string via the
	// shared ServerLaunchArguments builder. The C++ side forwards the resolved connection facts and never assembles
	// the none/oauth/token arg string itself. The result arrives as a `server-launch-args-result` routed through the
	// status sink. The token travels only over this loopback IPC (never argv, never logged) — same as the §1.4 model.
	// (Params are In-prefixed so InToken does not shadow the FUnrealMcpBridgeServer::Token member — C4458 under /WX.)
	if (!bClientConnected || !bHandshakeOk)
		return false;

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TypeServerLaunchArgs);
	Message->SetStringField(TEXT("requestId"), RequestId);
	Message->SetNumberField(TEXT("port"), InPort);
	Message->SetNumberField(TEXT("pluginTimeoutMs"), InPluginTimeoutMs);
	Message->SetStringField(TEXT("authMode"), InAuthMode);
	if (!InToken.IsEmpty())
		Message->SetStringField(TEXT("token"), InToken);
	if (!InAuthIssuer.IsEmpty())
		Message->SetStringField(TEXT("authIssuer"), InAuthIssuer);
	if (!InPublicUrl.IsEmpty())
		Message->SetStringField(TEXT("publicUrl"), InPublicUrl);
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
