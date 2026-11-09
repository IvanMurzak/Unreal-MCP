// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "UnrealMcpEditorDiagnostics.h"

#include <memory>
#include <string>
#include <vector>

/** The `diagnostics` source (docs/rql.md "Editor diagnostics"): the editor's current warnings and errors. */
namespace UnrealMcpRqlDiagnosticsLocal
{
	struct FDiagnosticRow
	{
		int64_t Sequence = 0;
		std::string Source;
		std::string Severity;
		std::string Message;
		int32_t Count = 1;
		std::string FirstSeen;
		std::string LastSeen;
	};
}

using namespace UnrealMcpRqlDiagnosticsLocal;

void UnrealMcpRqlAddDiagnostics(FUnrealMcpRqlSession& Session)
{
	rql::TypeRegistry& Registry = Session.Types().Registry();
	Registry.structure<FDiagnosticRow>("Diagnostic")
		.member("Sequence", &FDiagnosticRow::Sequence)
		.member("Source", &FDiagnosticRow::Source)
		.member("Severity", &FDiagnosticRow::Severity)
		.member("Message", &FDiagnosticRow::Message)
		.member("Count", &FDiagnosticRow::Count)
		.member("FirstSeen", &FDiagnosticRow::FirstSeen)
		.member("LastSeen", &FDiagnosticRow::LastSeen)
		.no_identity();
	const rql::TypeDescriptor* RowType = Registry.descriptor<FDiagnosticRow>();
	Session.Types().AddNativeType(RowType);

	Session.AddSource(TEXT("diagnostics"), RowType, /*bList*/ true,
		TEXT("the editor's current warnings and errors, oldest first (Sequence Source Severity Message Count FirstSeen LastSeen): Source "
			"Viewport (the level viewport's on-screen messages, renderer scene warnings included), Compiler (Blueprints that fail to "
			"compile), MessageLog:<page> (MapCheck, PIE, BlueprintLog, LoadErrors, ...), Log:<category> (Warning / Error log lines); "
			"Severity Error / Warning / Info. Check it before you call the work done: diagnostics | where Severity != \"Info\""),
		[RowType]()
	{
		FUnrealMcpEditorDiagnostics& Diagnostics = FUnrealMcpEditorDiagnostics::Get();
		Diagnostics.Refresh();
		std::vector<rql::Value> Rows;
		for (const FUnrealMcpDiagnostic& Item : Diagnostics.Snapshot())
		{
			auto Row = std::make_shared<FDiagnosticRow>();
			Row->Sequence = Item.Sequence;
			Row->Source = UnrealMcpRqlToStd(Item.Source);
			Row->Severity = UnrealMcpRqlToStd(Item.Severity);
			Row->Message = UnrealMcpRqlToStd(Item.Message);
			Row->Count = Item.Count;
			Row->FirstSeen = UnrealMcpRqlToStd(Item.FirstSeen.ToIso8601());
			Row->LastSeen = UnrealMcpRqlToStd(Item.LastSeen.ToIso8601());
			Rows.push_back(rql::Value::structure(RowType, std::static_pointer_cast<void>(Row)));
		}
		return rql::Value::list_of(std::move(Rows));
	});
}
