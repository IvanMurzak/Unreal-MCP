# ControlRig RQL module

RQL module for Unreal's Control Rig: author procedural animation rigs without C++ (look-at / aim, two-bone and full-body IK, traces, springs: any rig unit with its settings, links and rig variables) and wire them into Anim Blueprints (a Control Rig node before Output Pose, its inputs bound to Anim Blueprint variables set every frame, e.g. from the nearest target); query rigs, their graphs, variables and hierarchy, and Control Rig components.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.26.0 or later (the RQL runtime and operations these extensions use), installed in the same project.
- Engine plugins: `ControlRig`. The installer enables these dependencies. Full Body IK units (`PBIK`) are in the engine's
  experimental `FullBodyIK` plugin, which a project enables itself; `controlRigUnits` lists only the units of enabled plugins.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-control-rig "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIControlRig/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill.

| Kind | Names |
| --- | --- |
| Sources | `controlRigs` (rig assets), `controlRigUnits` (the rig units a rig can run: name, title, category, pins), `controlRig` (operations) |
| Operations (`call`) | Rigs: `CreateRig` (optionally from a character's mesh), `AddVariable`, `AddUnit`, `AddSetVariable`, `SetPin`, `Connect`, `RemoveNode`, `AddLookAt`, `AddTwoBoneIK`, `AddBone`, `AddControl`. While playing: `RigValues`. Anim Blueprints: `AddToAnimBlueprint`, `SetRigInput`, `TrackNearestActor` |
| Members | `ControlRigBlueprint.Nodes` (graph: units, execution order, pin values and links), `ControlRigBlueprint.Variables`, `ControlRigBlueprint.Elements`, `.Hierarchy`, `.Controls`, `.Bones`, `AnimBlueprint.ControlRigs` (its Control Rig nodes, pose neighbours, bound inputs), `ControlRigComponent.BoundRig` (the running rig) |

A head that turns smoothly toward the nearest orb, on the UE5 mannequin, in seven calls:

```
controlRig | call CreateRig("/Game/Rigs/CR_LookAt", "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple")
controlRig | call AddVariable("/Game/Rigs/CR_LookAt", "LookAtTarget", "Vector")
controlRig | call AddVariable("/Game/Rigs/CR_LookAt", "LookAtWeight", "float", "0")
controlRig | call AddLookAt("/Game/Rigs/CR_LookAt", "head", "@LookAtTarget", "@LookAtWeight")
controlRig | call AddToAnimBlueprint("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed", "/Game/Rigs/CR_LookAt")
controlRig | call TrackNearestActor("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed", "BP_Orb", "LookAtTarget", "LookAtWeight", 600, 4)
unreal.asset("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed") | call Compile()
```

- `CreateRig(path, mesh)` imports the mesh's bones and curves, so the rig drives that skeleton in an Anim Blueprint.
- `AddVariable` adds a rig variable; a public one is an input of the rig's Control Rig node.
- `AddLookAt(rig, bone, target, weight, axis, after)` adds an Aim on the bone (kept upright); a `"@Variable"` target is a
  world location, converted to rig space with a From World node.
- `AddToAnimBlueprint(abp, rig, after, bindVariables)` splices a Control Rig node into the AnimGraph's pose chain right
  before Output Pose (after a foot IK rig the template already runs) or after a named node, compiles the rig, and binds
  each public rig variable to an Anim Blueprint variable of the same name (added when missing); `SetRigInput` rebinds
  an input or sets a constant.
- `TrackNearestActor(abp, class, locationVar, weightVar, radius, speed)` adds the Event Blueprint Update Animation logic
  that moves the location variable to the nearest actor of the class in range (smoothed) and blends the weight in and out.

Any rig unit, by its `controlRigUnits` name, with settings and links:

```
controlRigUnits | where Category contains "Simulation" | select Name DisplayName Execute Pins
controlRig | call AddUnit("/Game/Rigs/CR_LookAt", "SpringInterpVectorV2", "Target=@LookAtTarget, Strength=8", "", "Smooth")
controlRig | call Connect("/Game/Rigs/CR_LookAt", "Smooth.Result", "LookAt_head_Target.Value")
controlRig | call SetPin("/Game/Rigs/CR_LookAt", "LookAt_head.Weight", "0.8")
unreal.asset("/Game/Rigs/CR_LookAt") | select Variables Nodes
```

Executing units run at the end of Forwards Solve (or after `After`); pure units run when their outputs are linked. Values
are `"@Variable"` / `"@Node.Pin"` links, element names for item pins (`"head"`, `"Control:Target"`), `"(0, 1, 0)"`, enum
names, or the pin's text form. A bare rig is built with `CreateRig(path)`, `AddBone`, `AddControl` and `AddTwoBoneIK`.

World traces work from a rig in an Anim Blueprint with no setting to enable, in rig space (a hit on the ground under the
character is a small negative Z, a miss is `(0, 0, 0)` with `bHit` false). To see what a rig computes while playing, store
a pin in a variable and read the running rig:

```
controlRig | call AddVariable("/Game/Rigs/CR_Feet", "HitL", "bool", "", false)
controlRig | call AddSetVariable("/Game/Rigs/CR_Feet", "HitL", "@TraceL.bHit")
editor | call StartPlay()
editor | call AdvancePlay(1.0, 0)
controlRig | call RigValues("CR_Feet")
```

`SetCurveValue` on a curve the rig lacks does nothing in the engine, so `AddUnit` and `SetPin` refuse it. The `unreal-rql`
skill's guide has a foot IK rig built from `AddUnit` calls.

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
