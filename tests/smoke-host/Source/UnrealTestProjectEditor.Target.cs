using UnrealBuildTool;

public class UnrealTestProjectEditorTarget : TargetRules
{
	public UnrealTestProjectEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UnrealTestProject");
	}
}
