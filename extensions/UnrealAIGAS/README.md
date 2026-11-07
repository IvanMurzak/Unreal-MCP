# GAS RQL module

Gameplay Ability System authoring for agents, without project C++: Gameplay Effects (cost, cooldown, damage via SetByCaller, periodic regeneration, stacking, tags), ability Blueprints with cost, cooldown, tags and projectiles, ability system components with attributes, starting abilities and effects, Enhanced Input bindings, and attribute checks in Play In Editor.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.26.0 or later (the RQL runtime and operations these extensions use), installed in the same project.
- Engine plugins: `GameplayAbilities`, `EnhancedInput` (input bindings), `GameplayTagsEditor` (new tags). The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIGAS/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill.

| Kind | Names |
| --- | --- |
| Sources | `gasAbilities`, `gasEffects`, `gasAttributeSets` (class defaults), `gasComponents` (the Ability System Components of the running game, or of the editor world), `gas` |
| Operations (`call`) | Setup: `EnableGameplayAbilities`. Effects: `CreateEffect`, `SetEffectDuration`, `AddModifier`, `RemoveModifiers`, `SetEffectTags`, `SetStacking`. Abilities: `CreateAbility`, `SetCost`, `SetCooldown`, `SetAbilityTags`, `SetAbilityProjectile`, `SetDefault`. Actors: `AddAbilitySystem`, `AddStartingAbility`, `AddStartingEffect`, `BindAbilityInput`, `AddAttributeDisplay`, `GrantAbility`. Play In Editor: `ActivateAbility`, `ApplyEffect`, `SetAttribute` |
| Members | `AbilitySystemComponent.GrantedAbilities`, `.Attributes`, `.OwnedTags`, `.ActiveEffects`; `GameplayEffect.Summary`; `GameplayAbility.Summary` |

```
gas | call CreateEffect("/Game/GAS/GE_Damage")
gas | call AddModifier("/Game/GAS/GE_Damage", "Damage", "Add", "SetByCaller")
gas | call CreateEffect("/Game/GAS/GE_ManaRegen", "Infinite", 1)
gas | call AddModifier("/Game/GAS/GE_ManaRegen", "Mana", "Add", "5")
gas | call CreateAbility("/Game/GAS/GA_Fireball")
gas | call SetCost("/Game/GAS/GA_Fireball", "Mana", 20)
gas | call SetCooldown("/Game/GAS/GA_Fireball", 3)
gas | call SetAbilityProjectile("/Game/GAS/GA_Fireball", "/Game/GAS/GE_Damage", 25)
gas | call AddAbilitySystem("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "Health=100, MaxHealth=100, Mana=100, MaxMana=100")
gas | call AddStartingEffect("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "/Game/GAS/GE_ManaRegen")
gas | call BindAbilityInput("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", "/Game/GAS/GA_Fireball", "/Game/GAS/IA_Fireball", "One")
editor | set IsPlaying = true
gas | call ActivateAbility("/Game/GAS/GA_Fireball")
gasComponents | select Owner.Label Attributes OwnedTags ActiveEffects
```

## Runtime classes (no project C++)

Attribute sets are C++ classes in the Gameplay Ability System. So that Blueprint-only projects (and agents) need no C++, the plugin's runtime module `UnrealAIGASRuntime` ships generic classes the operations configure with data:

| Class | What it does |
| --- | --- |
| `UnrealAIAttributeSet` | Health / MaxHealth, Mana / MaxMana, Stamina / MaxStamina (kept within 0 ... Max, replicated) and the meta attributes Damage and Healing: an effect that adds to Damage subtracts it from Health. All start at 100. |
| `UnrealAIAbilitySystemComponent` | An Ability System Component configured with data: `AttributeSets`, `InitialAttributes` (maximums first), `StartingAbilities`, `StartingEffects`, `AbilityInputs` (Enhanced Input action to ability, for the controlling player) and `InputMappingContext`; Blueprint events `OnAttributeChanged`, `OnOutOfHealth`; `GetAttributeValue`, `SetAttributeBaseValue`. Actors need no `IAbilitySystemInterface`. |
| `UnrealAIGameplayAbility` | On activation commits cost and cooldown, applies `EffectsOnSelf`, spawns `ProjectileClass` carrying `HitEffect` (with `HitMagnitude` as `SetByCaller.Damage`) and ends. A Blueprint child with its own ActivateAbility graph runs that instead. |
| `UnrealAIGASProjectile` | A straight-flying sphere that applies its effect to the first actor with an Ability System Component it hits. |
| `UnrealAIAttributeTextComponent` | World text such as `HP {Health} / {MaxHealth}` with the owner's live attribute values. |

Projects with their own C++ attribute sets use them the same way (`AddAbilitySystem(blueprint, "Armor=5", "MyAttributeSet")`). A packaged game that uses these classes keeps this plugin enabled for its game targets (the runtime module does not depend on Unreal MCP; the plugin itself depends on the Unreal MCP plugin).

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
