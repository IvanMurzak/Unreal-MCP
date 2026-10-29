using UnrealBuildTool;

public class UnrealTestProjectTarget : TargetRules
{
	public UnrealTestProjectTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UnrealTestProject");
	}
}
