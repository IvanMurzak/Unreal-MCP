// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Tools/UnrealMcpSourceTools.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Internationalization/Regex.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

#if WITH_UNREAL_MCP_LIVE_CODING
#include "ILiveCodingModule.h"
#include "Modules/ModuleManager.h"
#endif

/**
 * The C++ source / script tool family (docs/ARCHITECTURE.md §10 "source family", issue #18).
 *
 * Six native CORE tools — source-read / source-create-class / source-update / source-delete /
 * source-list / source-compile — declared via the §3.3 builder and run on the game-thread dispatcher
 * (§4). The defining value of the family is source-compile's structured `{file,line,severity,message}`
 * build report (§3): the machine-readable feedback loop an AI iterates against until the code builds.
 *
 * Every file operation is JAILED to the loaded project's `Source/` directory: ResolveJailedPath
 * canonicalizes the caller path, collapses `..`, and rejects anything that escapes (plus a best-effort
 * junction/symlink check). `Plugins/`, `Config/` and engine paths are never reachable.
 *
 * source-compile runs synchronously inside the handler (ARCHITECTURE §4 envisions async-chaining once
 * the registry grows a TFuture handler surface; until then a compile blocks the game thread for its
 * duration — fine for the headless test gate, and a no-op "up to date" build returns in seconds). It
 * prefers Live Coding when the editor is interactive and Live Coding is live, else invokes UBT on the
 * project's Editor target. A loaded editor holds its module DLL, so a plain UBT relink fails to write
 * it — therefore `success` (process return code 0) is reported SEPARATELY from `compileClean`
 * (zero compiler-error diagnostics): the AI feedback loop keys off compiler diagnostics, which are
 * emitted before the link stage and are unaffected by the DLL lock.
 */
namespace UnrealMcpSourceTools
{
	// --- Jail + path helpers --------------------------------------------------------------------

	FString GetProjectSourceRoot()
	{
		FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	FString ModuleApiMacro(const FString& ModuleName)
	{
		return ModuleName.ToUpper() + TEXT("_API");
	}

	FJailedPath ResolveJailedPath(const FString& JailRoot, const FString& InPath)
	{
		FJailedPath Out;

		FString Root = FPaths::ConvertRelativePathToFull(JailRoot);
		FPaths::NormalizeDirectoryName(Root);

		FString Cleaned = InPath;
		Cleaned.TrimStartAndEndInline();
		if (Cleaned.IsEmpty())
		{
			Out.Error = TEXT("path is empty.");
			return Out;
		}
		FPaths::NormalizeFilename(Cleaned); // backslashes -> forward slashes

		const FString Combined = FPaths::IsRelative(Cleaned) ? (Root / Cleaned) : Cleaned;
		FString Full = FPaths::ConvertRelativePathToFull(Combined); // collapses '.' and '..'
		FPaths::NormalizeFilename(Full);

		// Path-level jail: after collapsing traversal, the result must be the root itself or under it.
		if (!Full.Equals(Root, ESearchCase::IgnoreCase) && !FPaths::IsUnderDirectory(Full, Root))
		{
			Out.Error = FString::Printf(TEXT("'%s' escapes the project Source/ jail."), *InPath);
			return Out;
		}

		// Best-effort junction/symlink jail: resolve the deepest EXISTING ancestor's real on-disk path
		// (fixes case and, where the platform supports it, follows reparse points) and re-check
		// containment, so a junction placed INSIDE Source/ that targets outside is still rejected.
		// NOTE: this is a PATH-level check, so a TOCTOU window exists between it and the later file op
		// (a junction could be swapped in after we validate). UE's file API offers no handle-based
		// re-verify, and the documented threat model (a local AI editing project source) accepts it.
		{
			IFileManager& FM = IFileManager::Get();
			FString Existing = Full;
			while (!Existing.Equals(Root, ESearchCase::IgnoreCase)
				&& !FM.FileExists(*Existing) && !FM.DirectoryExists(*Existing))
			{
				const FString Parent = FPaths::GetPath(Existing);
				if (Parent.IsEmpty() || Parent.Equals(Existing, ESearchCase::IgnoreCase))
				{
					break;
				}
				Existing = Parent;
			}
			if (FM.FileExists(*Existing) || FM.DirectoryExists(*Existing))
			{
				FString OnDisk = FPaths::ConvertRelativePathToFull(FM.GetFilenameOnDisk(*Existing));
				FPaths::NormalizeFilename(OnDisk);
				FString RootOnDisk = FPaths::ConvertRelativePathToFull(FM.GetFilenameOnDisk(*Root));
				FPaths::NormalizeFilename(RootOnDisk);
				FPaths::NormalizeDirectoryName(RootOnDisk);
				if (!OnDisk.Equals(RootOnDisk, ESearchCase::IgnoreCase) && !FPaths::IsUnderDirectory(OnDisk, RootOnDisk))
				{
					Out.Error = FString::Printf(TEXT("'%s' resolves through a link that escapes the project Source/ jail."), *InPath);
					return Out;
				}
			}
		}

		Out.FullPath = Full;
		Out.RelPath = Full;
		FPaths::MakePathRelativeTo(Out.RelPath, *(Root + TEXT("/")));

		// Reject NTFS alternate-data-stream syntax ("Foo.cpp:stream"): a ':' in the path RELATIVE to the
		// jail root would otherwise let a write create/read an ADS that survives canonicalization. The
		// drive-letter colon lives in the jail root, not the remainder, so checking RelPath is safe.
		if (Out.RelPath.Contains(TEXT(":")))
		{
			Out.FullPath.Reset();
			Out.RelPath.Reset();
			Out.Error = FString::Printf(TEXT("'%s' contains an illegal ':' (alternate data stream)."), *InPath);
			return Out;
		}

		Out.bOk = true;
		return Out;
	}

	// --- Compiler-diagnostic parser -------------------------------------------------------------

	void ParseDiagnostics(const FString& BuildOutput, TArray<FSourceDiagnostic>& OutDiagnostics)
	{
		OutDiagnostics.Reset();

		TArray<FString> Lines;
		BuildOutput.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

		// MSVC:  C:\path\File.cpp(42): error C2065: 'Foo': undeclared identifier
		//        File.cpp(42,5): warning C4101: ...   (newer toolchains may add a column)
		//        File.cpp(1): fatal error C1083: ...   (a missing/typo'd #include — the optional
		//        "fatal " prefix is consumed and the severity normalized to "error")
		const FRegexPattern MsvcPattern(TEXT("^\\s*(.+?)\\((\\d+)(?:,\\d+)?\\)\\s*:\\s*(?:fatal\\s+)?(error|warning)\\s+(.+?)\\s*$"));
		// clang: File.cpp:42:5: error: ...   (also "File.cpp:1:10: fatal error: 'X.h' file not found")
		const FRegexPattern ClangPattern(TEXT("^\\s*(.+?):(\\d+):(?:\\d+:)?\\s*(?:fatal\\s+)?(error|warning):\\s*(.+?)\\s*$"));

		TSet<FString> Seen;
		auto AddMatch = [&OutDiagnostics, &Seen](const FString& File, const FString& LineStr, const FString& Sev, const FString& Msg)
		{
			FSourceDiagnostic D;
			D.File = File;
			D.File.TrimStartAndEndInline();
			D.Line = FCString::Atoi(*LineStr);
			D.Severity = Sev.ToLower();
			D.Message = Msg;
			D.Message.TrimStartAndEndInline();
			const FString Key = FString::Printf(TEXT("%s|%d|%s|%s"), *D.File, D.Line, *D.Severity, *D.Message);
			if (!Seen.Contains(Key))
			{
				Seen.Add(Key);
				OutDiagnostics.Add(MoveTemp(D));
			}
		};

		for (const FString& Line : Lines)
		{
			if (Line.IsEmpty())
			{
				continue;
			}
			FRegexMatcher Msvc(MsvcPattern, Line);
			if (Msvc.FindNext())
			{
				AddMatch(Msvc.GetCaptureGroup(1), Msvc.GetCaptureGroup(2), Msvc.GetCaptureGroup(3), Msvc.GetCaptureGroup(4));
				continue;
			}
			FRegexMatcher Clang(ClangPattern, Line);
			if (Clang.FindNext())
			{
				AddMatch(Clang.GetCaptureGroup(1), Clang.GetCaptureGroup(2), Clang.GetCaptureGroup(3), Clang.GetCaptureGroup(4));
			}
		}
	}

	// --- Local file-op helpers ------------------------------------------------------------------

	static const TCHAR* FileHeaderComment()
	{
		return TEXT("// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.\n")
		       TEXT("// See the LICENSE file in the repository root for more information.\n");
	}

	/** True if @p Name is a legal C++ identifier (so a generated class name can never inject syntax). */
	static bool IsValidIdentifier(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return false;
		}
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR Ch = Name[Index];
			const bool bAlpha = (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('a') && Ch <= TEXT('z')) || Ch == TEXT('_');
			const bool bDigit = (Ch >= TEXT('0') && Ch <= TEXT('9'));
			if (!bAlpha && !(bDigit && Index > 0))
			{
				return false;
			}
		}
		return true;
	}

	/** Resolved class-scaffold parameters for one of the supported parent kinds. */
	struct FClassTemplate
	{
		FString Prefix;        // "U" / "A" / "F"
		FString ParentSymbol;  // "UObject" / "AActor" / "UActorComponent" / "" (plain)
		FString ParentHeader;  // include path, empty for plain
		bool bUClass = false;
	};

	static bool ResolveClassTemplate(const FString& InParent, FClassTemplate& Out, FString& OutError)
	{
		const FString P = InParent.TrimStartAndEnd();
		if (P.IsEmpty() || P.Equals(TEXT("UObject"), ESearchCase::IgnoreCase) || P.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
		{
			Out = { TEXT("U"), TEXT("UObject"), TEXT("UObject/NoExportTypes.h"), true };
			return true;
		}
		if (P.Equals(TEXT("AActor"), ESearchCase::IgnoreCase) || P.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))
		{
			Out = { TEXT("A"), TEXT("AActor"), TEXT("GameFramework/Actor.h"), true };
			return true;
		}
		if (P.Equals(TEXT("UActorComponent"), ESearchCase::IgnoreCase) || P.Equals(TEXT("ActorComponent"), ESearchCase::IgnoreCase))
		{
			Out = { TEXT("U"), TEXT("UActorComponent"), TEXT("Components/ActorComponent.h"), true };
			return true;
		}
		if (P.Equals(TEXT("None"), ESearchCase::IgnoreCase) || P.Equals(TEXT("Empty"), ESearchCase::IgnoreCase) || P.Equals(TEXT("Plain"), ESearchCase::IgnoreCase))
		{
			Out = { TEXT("F"), FString(), FString(), false };
			return true;
		}
		OutError = FString::Printf(
			TEXT("Unsupported parentClass '%s'. Supported: UObject, Actor, ActorComponent, None (plain class)."), *InParent);
		return false;
	}

	static FString BuildHeader(const FClassTemplate& Tpl, const FString& ClassName, const FString& ApiMacro)
	{
		const FString Symbol = Tpl.Prefix + ClassName;
		if (Tpl.bUClass)
		{
			return FString::Printf(
				TEXT("%s\n#pragma once\n\n#include \"CoreMinimal.h\"\n#include \"%s\"\n#include \"%s.generated.h\"\n\n")
				TEXT("UCLASS()\nclass %s %s : public %s\n{\n\tGENERATED_BODY()\n};\n"),
				FileHeaderComment(), *Tpl.ParentHeader, *ClassName, *ApiMacro, *Symbol, *Tpl.ParentSymbol);
		}
		return FString::Printf(
			TEXT("%s\n#pragma once\n\n#include \"CoreMinimal.h\"\n\nclass %s %s\n{\n};\n"),
			FileHeaderComment(), *ApiMacro, *Symbol);
	}

	static FString BuildCpp(const FString& ClassName)
	{
		return FString::Printf(TEXT("%s\n#include \"%s.h\"\n"), FileHeaderComment(), *ClassName);
	}

	// --- Tool registration ----------------------------------------------------------------------

	void Register(FUnrealMcpToolRegistry& Registry)
	{
		// source-read ------------------------------------------------------------------------------
		Registry.Tool(TEXT("source-read"))
			.Title(TEXT("Read Source File"))
			.Description(TEXT("Read a project C++/C# source file. 'path' is relative to the project Source/ "
			                  "directory (e.g. 'MyGame/MyActor.cpp') or an absolute path inside it; all access is "
			                  "JAILED to <Project>/Source. Optionally window the result by 1-based 'startLine'/"
			                  "'endLine' (inclusive). The returned text is byte-capped by 'maxBytes' (default "
			                  "262144, hard cap 4194304); 'truncated' reports whether the cap clipped it."))
			.ParamString(TEXT("path"), TEXT("Source file path relative to <Project>/Source (or absolute, inside it)."), EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("startLine"), TEXT("1-based first line to return (inclusive). Default 1."))
			.ParamInt(TEXT("endLine"), TEXT("1-based last line to return (inclusive). Default: end of file."))
			.ParamInt(TEXT("maxBytes"), TEXT("Maximum bytes of content to return (1..4194304). Default 262144."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				const FJailedPath Jailed = ResolveJailedPath(GetProjectSourceRoot(), Path);
				if (!Jailed.bOk)
				{
					return FUnrealMcpToolResult::Error(Jailed.Error);
				}
				if (!FPaths::FileExists(Jailed.FullPath))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No source file at '%s'."), *Jailed.RelPath));
				}

				// Refuse absurdly large files BEFORE reading the whole thing into memory — the byte cap only
				// bounds the RETURNED slice, not the read. 64 MiB is far above any sane source file.
				static constexpr int64 MaxReadableBytes = 64ll * 1024 * 1024;
				const int64 OnDiskSize = IFileManager::Get().FileSize(*Jailed.FullPath);
				if (OnDiskSize > MaxReadableBytes)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("'%s' is %lld bytes; refusing to read files larger than %lld bytes."),
						*Jailed.RelPath, OnDiskSize, MaxReadableBytes));
				}

				FString Content;
				if (!FFileHelper::LoadFileToString(Content, *Jailed.FullPath))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to read '%s'."), *Jailed.RelPath));
				}

				TArray<FString> AllLines;
				Content.ParseIntoArrayLines(AllLines, /*InCullEmpty*/ false);
				const int32 TotalLines = AllLines.Num();

				const bool bWindowed = Call.Has(TEXT("startLine")) || Call.Has(TEXT("endLine"));
				int32 StartLine = 1;
				int32 EndLine = TotalLines;
				if (bWindowed)
				{
					// Clamp in int64 BEFORE narrowing — a huge value (e.g. 4294967297) would otherwise wrap
					// to a small in-range int32 before the clamp ever ran.
					StartLine = (int32)FMath::Clamp<int64>(Call.GetInt(TEXT("startLine"), 1), 1, FMath::Max(1, TotalLines));
					EndLine = (int32)FMath::Clamp<int64>(Call.GetInt(TEXT("endLine"), TotalLines), StartLine, FMath::Max(1, TotalLines));

					TArray<FString> Window;
					for (int32 Index = StartLine - 1; Index < EndLine && Index < TotalLines; ++Index)
					{
						Window.Add(AllLines[Index]);
					}
					Content = FString::Join(Window, TEXT("\n"));
				}

				// Byte-cap on the UTF-8 encoding (binary-search the longest prefix that fits).
				const int32 MaxBytes = (int32)FMath::Clamp<int64>(Call.GetInt(TEXT("maxBytes"), 262144), 1, 4194304);
				auto Utf8Len = [](const FString& S) { return FTCHARToUTF8(*S).Length(); };
				bool bTruncated = false;
				if (Utf8Len(Content) > MaxBytes)
				{
					int32 Lo = 0;
					int32 Hi = Content.Len();
					while (Lo < Hi)
					{
						const int32 Mid = (Lo + Hi + 1) / 2;
						if (Utf8Len(Content.Left(Mid)) <= MaxBytes) { Lo = Mid; } else { Hi = Mid - 1; }
					}
					// Don't slice between a UTF-16 surrogate pair: if the last kept code unit is a high
					// surrogate (U+D800..U+DBFF) its low half is at index Lo (excluded), so drop it too.
					if (Lo > 0 && Lo < Content.Len())
					{
						const TCHAR Prev = Content[Lo - 1];
						if (Prev >= (TCHAR)0xD800 && Prev <= (TCHAR)0xDBFF) { --Lo; }
					}
					Content = Content.Left(Lo);
					bTruncated = true;
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Jailed.RelPath);
				Structured->SetStringField(TEXT("content"), Content);
				Structured->SetNumberField(TEXT("totalLines"), TotalLines);
				Structured->SetNumberField(TEXT("startLine"), StartLine);
				Structured->SetNumberField(TEXT("endLine"), bWindowed ? EndLine : TotalLines);
				Structured->SetNumberField(TEXT("returnedBytes"), Utf8Len(Content));
				Structured->SetBoolField(TEXT("truncated"), bTruncated);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Read '%s' (%d line(s)%s)."), *Jailed.RelPath, TotalLines, bTruncated ? TEXT(", truncated") : TEXT("")),
					Structured);
			});

		// source-create-class ----------------------------------------------------------------------
		Registry.Tool(TEXT("source-create-class"))
			.Title(TEXT("Create C++ Class"))
			.Description(TEXT("Scaffold a new C++ class — header + cpp generated from templates into "
			                  "Source/<module>/, mirroring the editor's 'New C++ Class'. 'className' is the bare "
			                  "name WITHOUT the U/A/F prefix (the prefix is derived from the parent). 'parentClass' "
			                  "is one of: UObject (default), Actor, ActorComponent, None (a plain non-UCLASS class). "
			                  "'module' defaults to the project's primary module. Refuses to overwrite existing "
			                  "files unless 'force':true. All writes are JAILED to <Project>/Source."))
			.ParamString(TEXT("className"), TEXT("Bare class name (no U/A/F prefix), e.g. 'MyActor'."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("parentClass"), TEXT("Parent kind: UObject (default), Actor, ActorComponent, or None."))
			.ParamString(TEXT("module"), TEXT("Target module folder under Source/. Defaults to the project's primary module."))
			.ParamBool(TEXT("force"), TEXT("Overwrite existing header/cpp at the destination. Default false."))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ClassName = Call.GetString(TEXT("className"));
				if (!IsValidIdentifier(ClassName))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s' is not a valid C++ class name (letters/digits/underscore, not leading digit; no prefix)."), *ClassName));
				}
				FString Module = Call.GetString(TEXT("module"));
				if (Module.IsEmpty())
				{
					Module = FApp::GetProjectName();
				}
				if (!IsValidIdentifier(Module))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a valid module name."), *Module));
				}

				FClassTemplate Tpl;
				FString TemplateError;
				if (!ResolveClassTemplate(Call.GetString(TEXT("parentClass")), Tpl, TemplateError))
				{
					return FUnrealMcpToolResult::Error(TemplateError);
				}

				const FString Root = GetProjectSourceRoot();
				const FJailedPath ModuleDir = ResolveJailedPath(Root, Module);
				if (!ModuleDir.bOk)
				{
					return FUnrealMcpToolResult::Error(ModuleDir.Error);
				}
				if (!FPaths::DirectoryExists(ModuleDir.FullPath))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Module folder '%s' does not exist under Source/. Create the module first."), *Module));
				}

				const FJailedPath HeaderPath = ResolveJailedPath(Root, Module / (ClassName + TEXT(".h")));
				const FJailedPath CppPath = ResolveJailedPath(Root, Module / (ClassName + TEXT(".cpp")));
				if (!HeaderPath.bOk || !CppPath.bOk)
				{
					return FUnrealMcpToolResult::Error(HeaderPath.bOk ? CppPath.Error : HeaderPath.Error);
				}

				const bool bForce = Call.GetBool(TEXT("force"), false);
				if (!bForce && (FPaths::FileExists(HeaderPath.FullPath) || FPaths::FileExists(CppPath.FullPath)))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s.h'/'%s.cpp' already exist in module '%s'. Pass 'force':true to overwrite."), *ClassName, *ClassName, *Module));
				}

				const FString ApiMacro = ModuleApiMacro(Module);
				const FString HeaderText = BuildHeader(Tpl, ClassName, ApiMacro);
				const FString CppText = BuildCpp(ClassName);
				if (!FFileHelper::SaveStringToFile(HeaderText, *HeaderPath.FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to write header '%s'."), *HeaderPath.RelPath));
				}
				if (!FFileHelper::SaveStringToFile(CppText, *CppPath.FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to write cpp '%s'."), *CppPath.RelPath));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("className"), Tpl.Prefix + ClassName);
				Structured->SetStringField(TEXT("module"), Module);
				Structured->SetStringField(TEXT("parentClass"), Tpl.bUClass ? Tpl.ParentSymbol : TEXT("(none)"));
				Structured->SetStringField(TEXT("header"), HeaderPath.RelPath);
				Structured->SetStringField(TEXT("cpp"), CppPath.RelPath);
				Structured->SetBoolField(TEXT("isUClass"), Tpl.bUClass);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Created %s%s (%s.h + %s.cpp) in module '%s'. Run source-compile to build it."),
						*Tpl.Prefix, *ClassName, *ClassName, *ClassName, *Module),
					Structured);
			});

		// source-update ----------------------------------------------------------------------------
		Registry.Tool(TEXT("source-update"))
			.Title(TEXT("Update Source File"))
			.Description(TEXT("Replace an existing source file's full contents, or a 1-based inclusive line range. "
			                  "Pass 'content' (the replacement text). Omit 'startLine'/'endLine' to replace the "
			                  "whole file; pass both to splice 'content' over that line range. The file must already "
			                  "exist (use source-create-class for new files). JAILED to <Project>/Source."))
			.ParamString(TEXT("path"), TEXT("Source file path relative to <Project>/Source (or absolute, inside it)."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("content"), TEXT("Replacement text."), EUnrealMcpParamRequirement::Required)
			.ParamInt(TEXT("startLine"), TEXT("1-based first line to replace (inclusive). Requires 'endLine'."))
			.ParamInt(TEXT("endLine"), TEXT("1-based last line to replace (inclusive). Requires 'startLine'."))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				if (!Call.Has(TEXT("content")))
				{
					return FUnrealMcpToolResult::Error(TEXT("'content' is required."));
				}
				const FString NewContent = Call.GetString(TEXT("content"));

				const FJailedPath Jailed = ResolveJailedPath(GetProjectSourceRoot(), Path);
				if (!Jailed.bOk)
				{
					return FUnrealMcpToolResult::Error(Jailed.Error);
				}
				if (!FPaths::FileExists(Jailed.FullPath))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No source file at '%s' (use source-create-class to create new files)."), *Jailed.RelPath));
				}

				const bool bRange = Call.Has(TEXT("startLine")) || Call.Has(TEXT("endLine"));
				FString Final;
				FString Mode;
				if (bRange)
				{
					if (!Call.Has(TEXT("startLine")) || !Call.Has(TEXT("endLine")))
					{
						return FUnrealMcpToolResult::Error(TEXT("A line-range update requires BOTH 'startLine' and 'endLine'."));
					}
					FString Existing;
					if (!FFileHelper::LoadFileToString(Existing, *Jailed.FullPath))
					{
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to read '%s'."), *Jailed.RelPath));
					}
					// Preserve the file's dominant line ending and trailing newline so a one-line splice
					// produces a one-line diff, not a whole-file CRLF->LF rewrite that drops the final EOL.
					const TCHAR* Eol = Existing.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
					const bool bTrailingNewline = Existing.EndsWith(TEXT("\n"));
					TArray<FString> Lines;
					Existing.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);
					const int32 StartLine = (int32)Call.GetInt(TEXT("startLine"), 1);
					const int32 EndLine = (int32)Call.GetInt(TEXT("endLine"), 1);
					if (StartLine < 1 || EndLine < StartLine || StartLine > Lines.Num() || EndLine > Lines.Num())
					{
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("Invalid line range [%d..%d] for '%s' (%d line(s))."), StartLine, EndLine, *Jailed.RelPath, Lines.Num()));
					}
					TArray<FString> Replacement;
					NewContent.ParseIntoArrayLines(Replacement, /*InCullEmpty*/ false);
					TArray<FString> Result;
					Result.Append(Lines.GetData(), StartLine - 1);
					Result.Append(Replacement);
					for (int32 Index = EndLine; Index < Lines.Num(); ++Index)
					{
						Result.Add(Lines[Index]);
					}
					Final = FString::Join(Result, Eol);
					if (bTrailingNewline)
					{
						Final += Eol;
					}
					Mode = FString::Printf(TEXT("range [%d..%d]"), StartLine, EndLine);
				}
				else
				{
					Final = NewContent;
					Mode = TEXT("full");
				}

				if (!FFileHelper::SaveStringToFile(Final, *Jailed.FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to write '%s'."), *Jailed.RelPath));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Jailed.RelPath);
				Structured->SetStringField(TEXT("mode"), bRange ? TEXT("range") : TEXT("full"));
				Structured->SetNumberField(TEXT("bytesWritten"), FTCHARToUTF8(*Final).Length());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Updated '%s' (%s)."), *Jailed.RelPath, *Mode), Structured);
			});

		// source-delete ----------------------------------------------------------------------------
		Registry.Tool(TEXT("source-delete"))
			.Title(TEXT("Delete Source File"))
			.Description(TEXT("Delete a source file. JAILED to <Project>/Source — refuses directories and any path "
			                  "outside the jail. Destructive and not undoable from MCP."))
			.ParamString(TEXT("path"), TEXT("Source file path relative to <Project>/Source (or absolute, inside it)."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				const FJailedPath Jailed = ResolveJailedPath(GetProjectSourceRoot(), Path);
				if (!Jailed.bOk)
				{
					return FUnrealMcpToolResult::Error(Jailed.Error);
				}
				if (FPaths::DirectoryExists(Jailed.FullPath))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s' is a directory; source-delete only removes files."), *Jailed.RelPath));
				}
				if (!FPaths::FileExists(Jailed.FullPath))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No source file at '%s'."), *Jailed.RelPath));
				}
				if (!IFileManager::Get().Delete(*Jailed.FullPath, /*RequireExists*/ false, /*EvenReadOnly*/ true))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to delete '%s'."), *Jailed.RelPath));
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Jailed.RelPath);
				Structured->SetBoolField(TEXT("deleted"), true);
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Deleted '%s'."), *Jailed.RelPath), Structured);
			});

		// source-list ------------------------------------------------------------------------------
		Registry.Tool(TEXT("source-list"))
			.Title(TEXT("List Source Files"))
			.Description(TEXT("Enumerate source files with their byte sizes. Scoped to 'module' (a folder under "
			                  "Source/) when given, else the whole <Project>/Source tree. 'recursive' (default true) "
			                  "walks sub-folders. By default lists .h/.cpp/.cs; pass 'extensions' (array of extensions "
			                  "without the dot) to override. JAILED to <Project>/Source."))
			.ParamString(TEXT("module"), TEXT("Module folder under Source/ to scope the listing. Omit for the whole tree."))
			.ParamBool(TEXT("recursive"), TEXT("Recurse sub-folders. Default true."))
			.Param(TEXT("extensions"), TEXT("array"), TEXT("Extensions to include (without the dot). Default ['h','cpp','cs']."),
				EUnrealMcpParamRequirement::Optional, nullptr)
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Root = GetProjectSourceRoot();
				FString BaseDir = Root;
				FString BaseRel;
				const FString Module = Call.GetString(TEXT("module"));
				if (!Module.IsEmpty())
				{
					const FJailedPath Jailed = ResolveJailedPath(Root, Module);
					if (!Jailed.bOk)
					{
						return FUnrealMcpToolResult::Error(Jailed.Error);
					}
					if (!FPaths::DirectoryExists(Jailed.FullPath))
					{
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Module folder '%s' does not exist under Source/."), *Module));
					}
					BaseDir = Jailed.FullPath;
					BaseRel = Jailed.RelPath;
				}

				TArray<FString> Extensions;
				const TArray<TSharedPtr<FJsonValue>>* ExtArray = nullptr;
				if (Call.Arguments->TryGetArrayField(TEXT("extensions"), ExtArray))
				{
					for (const TSharedPtr<FJsonValue>& Value : *ExtArray)
					{
						FString Ext;
						if (Value.IsValid() && Value->TryGetString(Ext) && !Ext.IsEmpty())
						{
							Extensions.AddUnique(Ext.Replace(TEXT("."), TEXT("")).ToLower());
						}
					}
				}
				if (Extensions.Num() == 0)
				{
					Extensions = { TEXT("h"), TEXT("cpp"), TEXT("cs") };
				}

				const bool bRecursive = Call.GetBool(TEXT("recursive"), true);
				IFileManager& FM = IFileManager::Get();
				TArray<FString> Found;
				for (const FString& Ext : Extensions)
				{
					TArray<FString> Matches;
					const FString Wildcard = FString::Printf(TEXT("*.%s"), *Ext);
					if (bRecursive)
					{
						FM.FindFilesRecursive(Matches, *BaseDir, *Wildcard, /*Files*/ true, /*Dirs*/ false, /*bClearFileNames*/ false);
					}
					else
					{
						TArray<FString> Names;
						FM.FindFiles(Names, *(BaseDir / Wildcard), /*Files*/ true, /*Dirs*/ false);
						for (const FString& Name : Names)
						{
							Matches.Add(BaseDir / Name);
						}
					}
					Found.Append(Matches);
				}

				Found.Sort();
				int64 TotalBytes = 0;
				TArray<TSharedPtr<FJsonValue>> Files;
				for (const FString& Full : Found)
				{
					FString Rel = FPaths::ConvertRelativePathToFull(Full);
					FPaths::NormalizeFilename(Rel);
					FPaths::MakePathRelativeTo(Rel, *(Root + TEXT("/")));
					const int64 Bytes = FM.FileSize(*Full);
					if (Bytes >= 0)
					{
						TotalBytes += Bytes;
					}
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("path"), Rel);
					Entry->SetNumberField(TEXT("bytes"), (double)Bytes);
					Files.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("root"), Module.IsEmpty() ? TEXT("Source") : (TEXT("Source/") + BaseRel));
				Structured->SetNumberField(TEXT("count"), Files.Num());
				Structured->SetNumberField(TEXT("totalBytes"), (double)TotalBytes);
				Structured->SetArrayField(TEXT("files"), Files);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Listed %d source file(s) (%lld bytes)."), Files.Num(), TotalBytes), Structured);
			});

		// source-compile ---------------------------------------------------------------------------
		Registry.Tool(TEXT("source-compile"))
			.Title(TEXT("Compile Project C++"))
			.Description(TEXT("Compile the project's C++ and return a STRUCTURED diagnostic report — the AI feedback "
			                  "loop. Prefers Live Coding when the editor is interactive and Live Coding is active, "
			                  "otherwise invokes UnrealBuildTool on the project's Editor target ('target' overrides; "
			                  "default '<Project>Editor'). The result carries 'diagnostics' (an array of "
			                  "{file,line,severity,message}), 'errorCount'/'warningCount', the process 'returnCode', "
			                  "'success' (returnCode==0) and 'compileClean' (zero compiler errors — true even when a "
			                  "running editor holds the module DLL so the link stage cannot relink it). Set "
			                  "'useLiveCoding':false to force the UBT path."))
			.ParamString(TEXT("target"), TEXT("Build target name. Default '<Project>Editor'."))
			.ParamString(TEXT("configuration"), TEXT("Build configuration. Default 'Development'."))
			.ParamString(TEXT("platform"), TEXT("Build platform. Default the host platform (e.g. 'Win64')."))
			.ParamBool(TEXT("useLiveCoding"), TEXT("Prefer Live Coding when available (interactive editor only). Default true."))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const bool bUseLiveCoding = Call.GetBool(TEXT("useLiveCoding"), true);

				const FString ProjectName = FApp::GetProjectName();
				FString TargetName = Call.GetString(TEXT("target"));
				if (TargetName.IsEmpty())
				{
					TargetName = ProjectName + TEXT("Editor");
				}

#if WITH_UNREAL_MCP_LIVE_CODING
				// Live Coding patches the RUNNING editor in place — the only way to apply C++ changes
				// without relinking the locked module DLL. Only viable interactively (the console must be
				// up); headless/unattended runs fall through to UBT.
				if (bUseLiveCoding && !FApp::IsUnattended())
				{
					ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(FName(LIVE_CODING_MODULE_NAME));
					if (LiveCoding && LiveCoding->IsEnabledForSession() && LiveCoding->HasStarted() && !LiveCoding->IsCompiling())
					{
						ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
						const bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);
						const bool bLcSuccess = bStarted
							&& (Result == ELiveCodingCompileResult::Success || Result == ELiveCodingCompileResult::NoChanges);

						const TCHAR* ResultText = TEXT("Unknown");
						switch (Result)
						{
							case ELiveCodingCompileResult::Success: ResultText = TEXT("Success"); break;
							case ELiveCodingCompileResult::NoChanges: ResultText = TEXT("NoChanges"); break;
							case ELiveCodingCompileResult::InProgress: ResultText = TEXT("InProgress"); break;
							case ELiveCodingCompileResult::CompileStillActive: ResultText = TEXT("CompileStillActive"); break;
							case ELiveCodingCompileResult::NotStarted: ResultText = TEXT("NotStarted"); break;
							case ELiveCodingCompileResult::Failure: ResultText = TEXT("Failure"); break;
							case ELiveCodingCompileResult::Cancelled: ResultText = TEXT("Cancelled"); break;
						}

						TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
						Structured->SetStringField(TEXT("method"), TEXT("live-coding"));
						Structured->SetStringField(TEXT("result"), ResultText);
						Structured->SetBoolField(TEXT("success"), bLcSuccess);
						Structured->SetBoolField(TEXT("compileClean"), bLcSuccess);
						Structured->SetNumberField(TEXT("errorCount"), bLcSuccess ? 0 : 1);
						Structured->SetNumberField(TEXT("warningCount"), 0);
						// Live Coding surfaces a coarse enum, not per-diagnostic rows; the UBT path carries
						// the full {file,line,severity,message} report.
						Structured->SetArrayField(TEXT("diagnostics"), {});
						// On a hard Failure, DON'T return the diagnostic-less enum — fall through to the UBT
						// path below for the full structured report (success/compileClean already model the
						// locked-DLL relink failure UBT will then hit).
						if (Result != ELiveCodingCompileResult::Failure)
						{
							return FUnrealMcpToolResult::Success(
								FString::Printf(TEXT("Live Coding compile: %s."), ResultText), Structured);
						}
					}
				}
#endif

				// UBT path -------------------------------------------------------------------------
				FString Platform = Call.GetString(TEXT("platform"));
				if (Platform.IsEmpty())
				{
					Platform = FPlatformProcess::GetBinariesSubdirectory();
				}
				FString Configuration = Call.GetString(TEXT("configuration"));
				if (Configuration.IsEmpty())
				{
					Configuration = TEXT("Development");
				}

				// target/platform/configuration are pasted verbatim into the UBT command line, so each must
				// be a single identifier-like token (no whitespace, quotes, or dash-prefixed flags) — else a
				// value like "UnrealTestProjectEditor Win64 Development -Clean" would inject arbitrary UBT
				// arguments (e.g. -Clean wipes Binaries/Intermediate). Mirrors IsValidIdentifier used for
				// generated class/module names. All real targets/platforms/configs are bare identifiers.
				if (!IsValidIdentifier(TargetName))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a valid build target name."), *TargetName));
				}
				if (!IsValidIdentifier(Platform))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a valid build platform."), *Platform));
				}
				if (!IsValidIdentifier(Configuration))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a valid build configuration."), *Configuration));
				}

				FString UProject;
				if (FPaths::IsProjectFilePathSet())
				{
					UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
				}
				else
				{
					UProject = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / (ProjectName + TEXT(".uproject")));
				}

#if PLATFORM_WINDOWS
				const FString Ubt = FPaths::ConvertRelativePathToFull(
					FPaths::EngineDir() / TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"));
#else
				const FString Ubt = FPaths::ConvertRelativePathToFull(
					FPaths::EngineDir() / TEXT("Build/BatchFiles/RunUBT.sh"));
#endif
				if (!FPaths::FileExists(Ubt))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("UnrealBuildTool not found at '%s'."), *Ubt));
				}

				const FString Params = FString::Printf(TEXT("%s %s %s -project=\"%s\" -WaitMutex"),
					*TargetName, *Platform, *Configuration, *UProject);

				int32 ReturnCode = -1;
				FString StdOut;
				FString StdErr;
				const double Start = FPlatformTime::Seconds();
				const bool bLaunched = FPlatformProcess::ExecProcess(*Ubt, *Params, &ReturnCode, &StdOut, &StdErr);
				const double Elapsed = FPlatformTime::Seconds() - Start;
				if (!bLaunched)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to launch UnrealBuildTool '%s'."), *Ubt));
				}

				const FString Combined = StdOut + TEXT("\n") + StdErr;
				TArray<FSourceDiagnostic> Diagnostics;
				ParseDiagnostics(Combined, Diagnostics);

				int32 ErrorCount = 0;
				int32 WarningCount = 0;
				TArray<TSharedPtr<FJsonValue>> DiagJson;
				for (const FSourceDiagnostic& Diag : Diagnostics)
				{
					if (Diag.Severity == TEXT("error")) { ++ErrorCount; }
					else if (Diag.Severity == TEXT("warning")) { ++WarningCount; }
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("file"), Diag.File);
					Entry->SetNumberField(TEXT("line"), Diag.Line);
					Entry->SetStringField(TEXT("severity"), Diag.Severity);
					Entry->SetStringField(TEXT("message"), Diag.Message);
					DiagJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				const bool bSuccess = (ReturnCode == 0);
				const bool bCompileClean = (ErrorCount == 0);

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("method"), TEXT("ubt"));
				Structured->SetStringField(TEXT("target"), TargetName);
				Structured->SetStringField(TEXT("configuration"), Configuration);
				Structured->SetStringField(TEXT("platform"), Platform);
				Structured->SetNumberField(TEXT("returnCode"), ReturnCode);
				Structured->SetBoolField(TEXT("success"), bSuccess);
				Structured->SetBoolField(TEXT("compileClean"), bCompileClean);
				Structured->SetNumberField(TEXT("errorCount"), ErrorCount);
				Structured->SetNumberField(TEXT("warningCount"), WarningCount);
				Structured->SetNumberField(TEXT("durationSeconds"), Elapsed);
				Structured->SetArrayField(TEXT("diagnostics"), DiagJson);
				// A bounded tail of the raw output aids debugging when no diagnostic matched (e.g. a link
				// failure, or a UBT configuration error) without flooding the response.
				Structured->SetStringField(TEXT("outputTail"), Combined.Right(4000));

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Compile (%s, UBT): %d error(s), %d warning(s); return code %d in %.0fs."),
						*TargetName, ErrorCount, WarningCount, ReturnCode, Elapsed),
					Structured);
			});
	}
}
