using UnrealBuildTool;

public class UnrealMcpTests : ModuleRules
{
    public UnrealMcpTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
    }
}
