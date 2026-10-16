using UnrealBuildTool;

public class UnrealMcpTestsEditorTarget : TargetRules
{
    public UnrealMcpTestsEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("UnrealMcpTests");
    }
}
