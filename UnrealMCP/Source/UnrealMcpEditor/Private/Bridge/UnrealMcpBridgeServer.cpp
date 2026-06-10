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

int32 FUnrealMcpBridgeServer::Start(const FString& InToken, const FString& InProjectPath, const FString& InPluginVersion, const FString& InEngineVersion)
{
	Token = InToken;
	ProjectPath = InProjectPath;
	PluginVersion = InPluginVersion;
	EngineVersion = InEngineVersion;

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

	// One connection at a time: a fresh dial replaces a stale one (§1.5 re-dial after reconnect).
	{
		FScopeLock Lock(&ConnectionMutex);
		if (ClientSocket != nullptr)
		{
			ClientSocket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
			ClientSocket = nullptr;
		}
		InSocket->SetNonBlocking(true);
		ClientSocket = InSocket;
	}

	bHandshakeOk = false;
	bClientConnected = true;
	LastActivitySeconds.Set(static_cast<int32>(FPlatformTime::Seconds()));
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] sidecar connected from %s; awaiting handshake."), *Endpoint.ToString());
	return true; // keep the socket — we own it now
}

bool FUnrealMcpBridgeServer::Init() { return true; }

uint32 FUnrealMcpBridgeServer::Run()
{
	FUnrealMcpNdjsonAccumulator Accumulator;
	int32 ServedGenerationSocket = 0;
	uint8 Buffer[64 * 1024];

	while (!bStopRequested)
	{
		FSocket* Sock;
		{
			FScopeLock Lock(&ConnectionMutex);
			Sock = ClientSocket;
		}

		if (Sock == nullptr)
		{
			FPlatformProcess::Sleep(0.05f);
			Accumulator = FUnrealMcpNdjsonAccumulator();
			continue;
		}

		if (!Sock->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200)))
			continue;

		int32 BytesRead = 0;
		const bool bRecvOk = Sock->Recv(Buffer, sizeof(Buffer), BytesRead);
		if (!bRecvOk || BytesRead == 0)
		{
			// Peer closed or errored — drop the connection and wait for a re-dial.
			CloseActiveConnection();
			Accumulator = FUnrealMcpNdjsonAccumulator();
			continue;
		}

		LastActivitySeconds.Set(static_cast<int32>(FPlatformTime::Seconds()));

		TArray<FString> Lines;
		if (!Accumulator.Push(Buffer, BytesRead, Lines))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] IPC line exceeded the frame cap; dropping connection."));
			CloseActiveConnection();
			Accumulator = FUnrealMcpNdjsonAccumulator();
			continue;
		}

		for (const FString& Line : Lines)
		{
			if (!Line.IsEmpty())
				HandleLine(Line);
		}
	}
	return 0;
}

void FUnrealMcpBridgeServer::Stop()
{
	bStopRequested = true;
}

void FUnrealMcpBridgeServer::Exit() {}

void FUnrealMcpBridgeServer::HandleLine(const FString& Line)
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

	if (Type == TypeHandshake)        HandleHandshake(Message);
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
	else
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] ignoring IPC message of type '%s'."), *Type);
	}
}

void FUnrealMcpBridgeServer::HandleHandshake(const TSharedPtr<FJsonObject>& Message)
{
	FString IncomingToken;
	Message->TryGetStringField(TEXT("token"), IncomingToken);

	if (Token.IsEmpty() || IncomingToken != Token)
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] handshake rejected (token mismatch); closing connection."));
		CloseActiveConnection();
		return;
	}

	bHandshakeOk = true;
	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] handshake accepted."));
	SendHandshakeAck();

	// §1.5: on Ready immediately push the manifest (config push lands with the connection-config task).
	{
		FScopeLock Lock(&WriteMutex);
		SendManifestLocked();
	}

	StartHeartbeat();
}

void FUnrealMcpBridgeServer::HandleToolCall(const TSharedPtr<FJsonObject>& Message)
{
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

	// Per-call cancel flag (§4), kept alive until the response is sent.
	TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> CancelFlag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);
	{
		FScopeLock Lock(&CancelMutex);
		CancelFlags.Add(RequestId, CancelFlag);
	}

	FUnrealMcpToolCall Call(Arguments);
	Call.CancelRequested = &CancelFlag.Get();

	const FString CapturedTool = ToolName;
	const FString CapturedRequestId = RequestId;

	Dispatcher.Dispatch(
		Call,
		[this, CapturedTool](const FUnrealMcpToolCall& C) { return Registry.Execute(CapturedTool, C); },
		FTimespan::FromMilliseconds(TimeoutMs))
		.Next([this, CapturedRequestId](FUnrealMcpToolResult Result)
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
			Response->SetArrayField(TEXT("content"), Content);

			if (Result.Structured.IsValid())
				Response->SetObjectField(TEXT("structured"), Result.Structured);

			SendMessage(Response);

			FScopeLock Lock(&CancelMutex);
			CancelFlags.Remove(CapturedRequestId);
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

	int32 TotalSent = 0;
	while (TotalSent < Framed.Num())
	{
		int32 JustSent = 0;
		if (!ClientSocket->Send(Framed.GetData() + TotalSent, Framed.Num() - TotalSent, JustSent) || JustSent <= 0)
			return false;
		TotalSent += JustSent;
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
	SendMessage(Ack);
}

void FUnrealMcpBridgeServer::SendManifestLocked()
{
	// Caller holds WriteMutex. Build under the game/registry-stable assumption (registry mutates only at
	// startup). Send the manifest line directly (we already hold the write lock).
	const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
	const FString Json = SerializeCondensed(Manifest);
	const TArray<uint8> Framed = FUnrealMcpNdjsonAccumulator::Encode(Json);

	FScopeLock ConnLock(&ConnectionMutex);
	if (ClientSocket == nullptr)
		return;

	int32 TotalSent = 0;
	while (TotalSent < Framed.Num())
	{
		int32 JustSent = 0;
		if (!ClientSocket->Send(Framed.GetData() + TotalSent, Framed.Num() - TotalSent, JustSent) || JustSent <= 0)
			return;
		TotalSent += JustSent;
	}
}

void FUnrealMcpBridgeServer::PushManifest()
{
	if (!bClientConnected || !bHandshakeOk)
		return;
	FScopeLock Lock(&WriteMutex);
	SendManifestLocked();
}

void FUnrealMcpBridgeServer::CloseActiveConnection()
{
	StopHeartbeat();
	FScopeLock Lock(&ConnectionMutex);
	if (ClientSocket != nullptr)
	{
		ClientSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
		ClientSocket = nullptr;
	}
	bClientConnected = false;
	bHandshakeOk = false;
}

void FUnrealMcpBridgeServer::StartHeartbeat()
{
	if (HeartbeatFuture.IsValid())
		return;

	bHeartbeatStop = false;
	HeartbeatFuture = Async(EAsyncExecution::Thread, [this]()
	{
		while (!bHeartbeatStop && !bStopRequested)
		{
			FPlatformProcess::Sleep(static_cast<float>(HeartbeatIntervalSeconds));
			if (bHeartbeatStop || bStopRequested || !bClientConnected || !bHandshakeOk)
				continue;

			const int32 Now = static_cast<int32>(FPlatformTime::Seconds());
			if (Now - LastActivitySeconds.GetValue() > HeartbeatTimeoutSeconds)
			{
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] sidecar silent > %ds; dropping connection."), HeartbeatTimeoutSeconds);
				CloseActiveConnection();
				return;
			}

			TSharedPtr<FJsonObject> Ping = MakeShared<FJsonObject>();
			Ping->SetStringField(TEXT("type"), TypePing);
			SendMessage(Ping);
		}
	});
}

void FUnrealMcpBridgeServer::StopHeartbeat()
{
	bHeartbeatStop = true;
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

	if (ReaderThread != nullptr)
	{
		ReaderThread->Kill(true);
		delete ReaderThread;
		ReaderThread = nullptr;
	}

	if (Listener != nullptr)
	{
		delete Listener;
		Listener = nullptr;
	}

	CloseActiveConnection();

	if (ListenSocket != nullptr)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}
}
