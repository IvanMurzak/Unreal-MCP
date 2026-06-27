// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpDescriptorHash.h"
#include "UnrealMcpSchema.h"   // UnrealMcpSerializeCondensed (the one shared condensed serializer)
#include "Misc/SecureHash.h"

FString UnrealMcpComputeDescriptorHash(const TSharedPtr<FJsonObject>& Descriptor)
{
	// Condensed (whitespace-free, stable order) serialization MINUS the mutable enabled/schemaHash fields — the
	// canonical form every registry hashed. A null descriptor (never happens in practice — call sites always pass
	// a freshly built ToDescriptorJson()) degrades to hashing the empty string.
	if (Descriptor.IsValid())
	{
		Descriptor->RemoveField(TEXT("enabled"));
		Descriptor->RemoveField(TEXT("schemaHash"));
	}
	const FString Canonical = UnrealMcpSerializeCondensed(Descriptor);

	FSHA1 Sha;
	const FTCHARToUTF8 Utf8(*Canonical);
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Digest[20];
	Sha.GetHash(Digest);
	return FString(TEXT("sha1:")) + BytesToHex(Digest, 20).ToLower();
}
