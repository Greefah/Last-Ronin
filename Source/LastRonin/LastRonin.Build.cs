using System.IO;
using UnrealBuildTool;

public class LastRonin : ModuleRules
{
    public LastRonin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "EnhancedInput",
            "GameplayAbilities",
            "GameplayTags",
            "GameplayTasks",
            "Niagara",
            "AIModule",
            "CoreOnline",
            "HTTP",
            "Json",
            "JsonUtilities",
            "UMG",
            "Slate",
            "SlateCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[] { });
    }
}