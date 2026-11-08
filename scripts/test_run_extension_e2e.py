from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import extension_release as release
import run_extension_e2e as e2e

COUNT = {'content': [{'type': 'text', 'text': '3'}], 'isError': False}
STRUCTURED_COUNT = {'content': [{'type': 'text', 'text': 'ok'}], 'structuredContent': {'result': {'kind': 'scalar', 'value': 0}}}
ESCAPED_COUNT = {'content': [{'type': 'text', 'text': json.dumps({'kind': 'scalar', 'value': 12})}]}
NO_COUNT = {'content': [{'type': 'text', 'text': 'no answer'}], 'isError': False}


def text_result(text):
    return {'content': [{'type': 'text', 'text': text}], 'isError': False}


def all_checks():
    return {(plugin, path.name): e2e.load_check(path) for plugin in release.components() for path in e2e.checks_for(plugin)}


def matches(check, result):
    try:
        e2e.evaluate(check, 0, json.dumps(result, indent=2))
        return True
    except ValueError:
        return False


def evaluates(check, code, output):
    try:
        e2e.evaluate(check, code, output if isinstance(output, str) else json.dumps(output, indent=2))
        return True
    except ValueError:
        return False


def receipt(method, result):
    return text_result(f'1 result\n$ref|method|result\nNiagaraOperations#4435|{method}|{result}')


SET_INPUT = 'SetModuleInput(NiagaraSystem System, string Emitter, string Module, string Input, string Value)'
SPAWN = 'SpawnSystem(NiagaraSystem System, Vector Location, string Label, bool bAutoActivate)'
COLLISION = 'EnableCollision(NiagaraSystem System, string Emitter, string Mode)'
PCG = 'PCGOperations#4854'
CONNECT = 'Connect(PCGGraph Graph, string From, string To, string FromPin, string ToPin)'
SETTING = 'SetNodeSetting(PCGGraph Graph, string Node, string Setting, string Value)'
GENERATE = 'Generate(Actor Actor, int Seed, double TimeoutSeconds)'


def pcg_receipt(row):
    return text_result(f'1 result\n$ref|method|result\n{row}')


def pcg_rows(method, *results):
    return text_result(f'{len(results)} results\nmethod: {method}\n$ref|result\n' + '\n'.join(f'{PCG}|{r}' for r in results))


# The Niagara and PCG authoring sequences: per check, an answer the live UE 5.7 editor gives and one that must not pass.
AUTHORING = {
    'niagara-templates-query.e2e.json': (
        text_result('1 result\nPath|Kind\n/Niagara/DefaultAssets/Templates/Emitters/Fountain|Emitter'), NO_COUNT),
    'niagara-authoring-1-create.e2e.json': (
        receipt('CreateSystem(string Path, string Template)', 'NiagaraSystem#1297(NS_FireSmoke)'), NO_COUNT),
    'niagara-authoring-2-smoke.e2e.json': (
        receipt('AddEmitter(NiagaraSystem System, NiagaraEmitter Emitter, string Name)', 'Smoke'),
        receipt('AddEmitter(NiagaraSystem System, NiagaraEmitter Emitter, string Name)', 'HangingParticulates')),
    'niagara-authoring-3-inputs.e2e.json': (receipt(SET_INPUT, '150'), receipt(SET_INPUT, '1500')),
    'niagara-authoring-4-collision.e2e.json': (
        receipt(COLLISION, 'Fountain: added Particle Update/Collision; CPU Collision Type = Ray Traced; note: a | '
                           'Smoke: added Particle Update/Collision; CPU Collision Type = Ray Traced; note: b'),
        receipt(COLLISION, 'Fountain: added Particle Update/Collision; CPU Collision Type = Ray Traced; note: a')),
    'niagara-authoring-5-compile.e2e.json': (
        receipt('Compile(NiagaraSystem System)', '{Succeeded:true,Errors:[],Warnings:[]}'),
        receipt('Compile(NiagaraSystem System)', '{Succeeded:false,Errors:[x],Warnings:[]}')),
    'niagara-authoring-6-place.e2e.json': (receipt(SPAWN, 'NiagaraActor#1580(E2E_Campfire)'), receipt(SPAWN, 'null')),
    'niagara-authoring-7-inputs-query.e2e.json': (
        text_result('1 result\nValue\nRay Traced'), text_result('1 result\nValue\nAnalytical Planes')),
    'niagara-authoring-8-unknown-emitter.e2e.json': (
        '{"error": "error call-threw: SetModuleInput threw: NS_FireSmoke has no emitter \'Fyre\' (emitters: Fountain, Smoke)"}',
        '{"error": "error call-threw: SetModuleInput threw: no system"}'),
    'niagara-authoring-9-cleanup-actor.e2e.json': (
        text_result('1 result\n$ref|method|result\nNiagaraActor#891004|Destroy()|null'), text_result('0 results')),
    'niagara-authoring-9-cleanup-asset.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|DeleteAsset(string Path, bool bForce)|null'), NO_COUNT),
    # The PCG forest sequence (a spline road, a slope / height filtered scatter on the host map's landscape, counts).
    'pcg-forest-01-road.e2e.json': (pcg_receipt('Editor@1|SpawnActor(string ClassName, ...)|Actor#6388(E2E_PCGRoad)'), NO_COUNT),
    'pcg-forest-02-road-spline.e2e.json': (
        pcg_receipt('Actor#6388|AddComponent(string ClassName, string Name)|SplineComponent#6389(Road)'), NO_COUNT),
    'pcg-forest-03-road-tag.e2e.json': (
        text_result('2 results\n$ref|member|old|new\nActor#6388|Tags|[]|[Road]\nActor#6388|Scale|(1, 1, 1)|(200, 1, 1)'), NO_COUNT),
    'pcg-forest-04-graph.e2e.json': (
        pcg_receipt(f'{PCG}|CreateGraph(string Path, string Template)|PCGGraph#6390(PCG_E2EForest)'), NO_COUNT),
    'pcg-forest-05-nodes.e2e.json': (
        pcg_rows('AddNode(PCGGraph Graph, string Type, string Name)', 'GetLandscapeData', 'SurfaceSampler', 'NormalToDensity',
                 'DensityFilter', 'AttributeFilterRange', 'GetSplineData', 'SplineSampler', 'BoundsModifier', 'Difference',
                 'SelfPruning', 'TransformPoints', 'StaticMeshSpawner'),
        pcg_rows('AddNode(PCGGraph Graph, string Type, string Name)', 'GetLandscapeData_0', 'SurfaceSampler_0')),
    'pcg-forest-06-connect.e2e.json': (
        pcg_rows(CONNECT, 'AttributeFilterRange.InsideFilter -> Difference.Source', 'BoundsModifier.Out -> Difference.Differences',
                 'StaticMeshSpawner.Out -> Output.Out'),
        pcg_rows(CONNECT, 'AttributeFilterRange.InsideFilter -> Difference.Source', 'BoundsModifier.Out -> Difference.Differences')),
    'pcg-forest-07-settings.e2e.json': (
        pcg_rows(SETTING, '0.75', 'Distance', '300', 'Set', '/Engine/BasicShapes/Cone: 3, /Engine/BasicShapes/Cylinder: 1'),
        pcg_rows(SETTING, '0.75', 'Distance', '300', 'Set', '/Engine/BasicShapes/Cone: 1')),
    'pcg-forest-08-parameter.e2e.json': (
        pcg_receipt(f'{PCG}|SetParameter(PCGGraph Graph, string Name, string Value, string Type)|0.01'),
        pcg_receipt(f'{PCG}|SetParameter(PCGGraph Graph, string Name, string Value, string Type)|0.1')),
    'pcg-forest-09-bind.e2e.json': (
        pcg_receipt(f'{PCG}|BindParameter(...)|Density -> SurfaceSampler.PointsPerSquaredMeter'), NO_COUNT),
    'pcg-forest-10-volume.e2e.json': (
        pcg_receipt(f'{PCG}|SpawnVolume(PCGGraph Graph, Vector Location, Vector Size, string Label)|PCGVolume#6404(E2E_PCGForest)'), NO_COUNT),
    'pcg-forest-11-generate.e2e.json': (
        pcg_receipt(f'{PCG}|{GENERATE}|{{Generated:true,Instances:338,Meshes:["Cone: 250","Cylinder: 88"],Points:338,Seed:42}}'),
        pcg_receipt(f'{PCG}|{GENERATE}|{{Generated:true,Instances:0,Meshes:[],Points:0,Seed:42}}')),
    'pcg-forest-12-variant.e2e.json': (
        pcg_receipt(f'{PCG}|{GENERATE}|{{Generated:true,Instances:336,Meshes:["Cone: 261"],Points:336,Seed:11}}'),
        pcg_receipt(f'{PCG}|{GENERATE}|{{Generated:true,Instances:336,Meshes:["Cone: 261"],Points:336,Seed:42}}')),
    'pcg-forest-13-counts.e2e.json': (
        text_result('1 result\nPCGComponent.Generated\n{Instances:336,Meshes:["Cone: 261","Cylinder: 75"],Points:336,IsGenerating:false}'),
        text_result('1 result\nPCGComponent.Generated\n{Instances:0,Meshes:[],Points:0,IsGenerating:false}')),
    'pcg-forest-14-graph-query.e2e.json': (
        text_result('1 result\nEdges|Parameters\n["BoundsModifier.Out -> Difference.Differences"]|[{Name:Density,Type:double,Value:"0.01"}]'),
        text_result('1 result\nEdges|Parameters\n["BoundsModifier.Out -> Difference.Source"]|[{Name:Density}]')),
    'pcg-forest-15-unknown-setting.e2e.json': (
        '{"error": "error call-threw: SetNodeSetting threw: DensityFilter has no setting \'Threshold\' (settings: LowerBound, UpperBound, bInvertFilter)"}',
        '{"error": "error call-threw: SetNodeSetting threw: PCG_E2EForest has no node \'DensityFilter\'"}'),
    'pcg-forest-16-cleanup-generated.e2e.json': (pcg_receipt(f'{PCG}|Cleanup(Actor Actor)|336'), NO_COUNT),
    'pcg-forest-17-cleanup-actors.e2e.json': (
        text_result('2 results\nmethod: Destroy()\n$ref|result\nActor#6388|null\nPCGVolume#6404|null'), text_result('0 results')),
    'pcg-forest-18-cleanup-asset.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|DeleteAsset(string Path, bool bForce)|null'), NO_COUNT),
}


def rows(count, method, results):
    return text_result(f'{count} result{"" if count == 1 else "s"}\nmethod: {method}\nresult\n' + '\n'.join(results))


def ei_receipt(method, result):
    return text_result(f'1 result\n$ref|method|result\nEnhancedInputOperations#7|{method}|{result}')


ADD_MAPPING = 'AddMapping(InputMappingContext Context, InputAction Action, string Key, string Modifiers, string Triggers)'
BIND = 'BindAction(Object Blueprint, InputAction Action, string Event, string Function, string Value)'
COMPILE = '1 result\n$ref|method|result\nBlueprint#{0}|Compile()|{{Succeeded:{1},Errors:{2},Warnings:0,Messages:[]}}'
OPERATIONS = ('EnhancedInputOperations : Object {} +methods: ' + ' | '.join(
    f'{name}(...) - x' for name in ('AddMapping', 'AddMappingContextOnPossess', 'BindAction', 'BindActionToggle', 'EnableUserSettings',
                                    'RemoveMappingContextOnEvent', 'SetActionTriggers', 'SetMappingTriggers', 'SetPlayerMappable')))
# The Enhanced Input control-scheme sequence (on foot + vehicle): per check, an answer of the live UE 5.7 editor's shape and
# one that must not pass.
SCHEME = {
    'enhanced-input-operations-schema.e2e.json': (text_result(OPERATIONS), text_result(OPERATIONS.replace('BindActionToggle', 'Other'))),
    'enhanced-input-scheme-01-actions.e2e.json': (
        rows(9, 'CreateAction(string Path, EInputActionValueType ValueType, string Triggers, string Modifiers)',
             [f'InputAction#{n}({a})' for n, a in enumerate(('IA_Move', 'IA_Look', 'IA_Jump', 'IA_Sprint', 'IA_Crouch', 'IA_Interact',
                                                              'IA_Throttle', 'IA_Steer', 'IA_Handbrake'))]), NO_COUNT),
    'enhanced-input-scheme-02-contexts.e2e.json': (
        rows(2, 'CreateContext(string Path)', ['InputMappingContext#1(IMC_OnFoot)', 'InputMappingContext#2(IMC_Vehicle)']),
        rows(1, 'CreateContext(string Path)', ['InputMappingContext#1(IMC_OnFoot)'])),
    'enhanced-input-scheme-03-move.e2e.json': (rows(5, ADD_MAPPING, ['true'] * 5), rows(4, ADD_MAPPING, ['true'] * 4)),
    'enhanced-input-scheme-04-on-foot.e2e.json': (rows(10, ADD_MAPPING, ['true'] * 10), rows(1, ADD_MAPPING, ['true'])),
    'enhanced-input-scheme-05-vehicle.e2e.json': (rows(11, ADD_MAPPING, ['true'] * 11), rows(1, ADD_MAPPING, ['true'])),
    'enhanced-input-scheme-06-hold.e2e.json': (
        ei_receipt('SetMappingTriggers(InputMappingContext Context, InputAction Action, string Key, string Triggers)', '1'),
        ei_receipt('SetMappingTriggers(InputMappingContext Context, InputAction Action, string Key, string Triggers)', '0')),
    'enhanced-input-scheme-07-mappable.e2e.json': (
        rows(7, 'SetPlayerMappable(InputAction Action, string DisplayName, string DisplayCategory, string Name)',
             ['IA_Jump', 'IA_Sprint', 'IA_Crouch', 'IA_Interact', 'IA_Throttle', 'IA_Steer', 'IA_Handbrake']), NO_COUNT),
    'enhanced-input-scheme-08-user-settings.e2e.json': (
        ei_receipt('EnableUserSettings(bool Enable)', 'Enable User Settings: false -> true (Config/DefaultInput.ini). Each local player ...'),
        ei_receipt('EnableUserSettings(bool Enable)', 'Enable User Settings: true -> false (Config/DefaultInput.ini). ')),
    'enhanced-input-scheme-09-hero.e2e.json': (text_result('Created Blueprint /Game/E2E/BP_E2E_Hero (parent Character)'), NO_COUNT),
    'enhanced-input-scheme-10-car.e2e.json': (text_result('Created Blueprint /Game/E2E/BP_E2E_Car (parent Pawn)'), NO_COUNT),
    'enhanced-input-scheme-11-possess.e2e.json': (
        rows(2, 'AddMappingContextOnPossess(Object Blueprint, InputMappingContext Context, int Priority)',
             ['K2Node_CallFunction#41', 'K2Node_CallFunction#57']),
        rows(1, 'AddMappingContextOnPossess(Object Blueprint, InputMappingContext Context, int Priority)', ['K2Node_CallFunction#41'])),
    'enhanced-input-scheme-12-sprint-start.e2e.json': (
        text_result('1 result\n$ref|method|result\nEdGraph#3(EventGraph)|AddCustomEvent(string Name, double X, double Y)|K2Node_CustomEvent#12'), NO_COUNT),
    'enhanced-input-scheme-13-sprint-stop.e2e.json': (
        text_result('1 result\n$ref|method|result\nEdGraph#3(EventGraph)|AddCustomEvent(string Name, double X, double Y)|K2Node_CustomEvent#13'), NO_COUNT),
    'enhanced-input-scheme-14-bind.e2e.json': (
        rows(4, BIND, [f'K2Node_CallFunction#{n}' for n in range(4)]), rows(3, BIND, [f'K2Node_CallFunction#{n}' for n in range(3)])),
    'enhanced-input-scheme-15-toggle.e2e.json': (
        ei_receipt('BindActionToggle(Object Blueprint, InputAction Action, string OnFunction, string OffFunction, string Event)', 'K2Node_MacroInstance#9'),
        ei_receipt('BindActionToggle(Object Blueprint, InputAction Action, string OnFunction, string OffFunction, string Event)', 'null')),
    'enhanced-input-scheme-16-car-bind.e2e.json': (ei_receipt(BIND, 'K2Node_CallFunction#77'), ei_receipt(BIND, 'null')),
    'enhanced-input-scheme-17-compile-hero.e2e.json': (
        text_result(COMPILE.format(1, 'true', 0)), text_result(COMPILE.format(1, 'false', 1))),
    'enhanced-input-scheme-18-compile-car.e2e.json': (
        text_result(COMPILE.format(2, 'true', 0)), text_result(COMPILE.format(2, 'false', 1))),
    'enhanced-input-scheme-19-readback-mappings.e2e.json': (
        text_result('1 result\nAction|Triggers|PlayerMappable|DisplayCategory\nIA_Interact|Hold(HoldTimeThreshold=0.3)|true|General'),
        text_result('1 result\nAction|Triggers|PlayerMappable|DisplayCategory\nIA_Interact||true|General')),
    'enhanced-input-scheme-20-readback-setup.e2e.json': (
        text_result('1 result\nInputSetup\n{Contexts:[{Event:ControllerChanged,Op:Remove,Context:/Game/E2E/Input/IMC_OnFoot},'
                    '{Event:ControllerChanged,Op:Add,Context:/Game/E2E/Input/IMC_OnFoot,Priority:0}],Actions:[{Action:IA_Jump,'
                    'Started:[Jump],Completed:[StopJumping]},{Action:IA_Sprint,Started:[StartSprint],Completed:[StopSprint]},'
                    '{Action:IA_Crouch,Started:[FlipFlop,Crouch,UnCrouch]}]}'),
        text_result('1 result\nInputSetup\n{Contexts:[],Actions:[{Action:IA_Jump,Started:[Jump],Completed:[StopJumping]}]}')),
    'enhanced-input-scheme-21-readback-mappable.e2e.json': (
        text_result('7 results\nName|PlayerMappable\nIA_Handbrake|{Name:IA_Handbrake,DisplayName:Handbrake,DisplayCategory:Vehicle}'),
        text_result('0 results')),
    'enhanced-input-scheme-22-unknown-trigger.e2e.json': (
        '{"error": "error call-threw: SetActionTriggers threw: \'DoubleTap\' is not an input trigger (input triggers: ChordAction, Combo, '
        'Down, Hold, HoldAndRelease, Pressed, Pulse, Released, RepeatedTap, Tap, or a Blueprint class path)"}',
        '{"error": "error call-threw: SetActionTriggers threw: no action"}'),
    'enhanced-input-scheme-99-cleanup-settings.e2e.json': (
        ei_receipt('EnableUserSettings(bool Enable)', 'Enable User Settings: true -> false (Config/DefaultInput.ini). '),
        ei_receipt('EnableUserSettings(bool Enable)', 'Enable User Settings: false -> true (Config/DefaultInput.ini). ')),
    'enhanced-input-scheme-99-cleanup-zassets.e2e.json': (
        rows(13, 'DeleteAsset(string Path, bool bForce)', ['null'] * 13), rows(12, 'DeleteAsset(string Path, bool bForce)', ['null'] * 12)),
}


def gas_receipt(method, result):
    return text_result(f'1 result\n$ref|method|result\nGASOperations#4512|{method}|{result}')


def gas_error(message):
    return json.dumps({'error': f'error call-threw: {message}'})


ACTIVATE = 'ActivateAbility(Class Ability, string Actor)'
MODIFIER = 'AddModifier(Class Effect, string Attribute, string Operation, string Magnitude)'
APPLY = 'ApplyEffect(Class Effect, string Actor, string SetByCaller, float Level)'
BIND = 'BindAbilityInput(Blueprint Blueprint, Class Ability, string InputAction, string Key, InputMappingContext MappingContext)'
# The GAS fireball sequence (authoring, then Play In Editor): per check, an answer the live UE 5.7 editor gives and one
# that must not pass.
GAS_AUTHORING = {
    'gas-authoring-00-preclean.e2e.json': (
        text_result('0 results'), text_result('error unknown-source: unreal.assets')),
    'gas-authoring-01-damage-effect.e2e.json': (
        gas_receipt('CreateEffect(string Path, string Duration, float Period)', 'Blueprint#2210(GE_Damage)'), NO_COUNT),
    'gas-authoring-02-damage-modifier.e2e.json': (gas_receipt(MODIFIER, '1'), gas_receipt(MODIFIER, '12')),
    'gas-authoring-03-regen-effect.e2e.json': (
        gas_receipt('CreateEffect(string Path, string Duration, float Period)', 'Blueprint#2214(GE_ManaRegen)'), NO_COUNT),
    'gas-authoring-04-regen-modifier.e2e.json': (gas_receipt(MODIFIER, '1'), gas_receipt(MODIFIER, '2')),
    'gas-authoring-05-ability.e2e.json': (
        gas_receipt('CreateAbility(string Path, Class Parent)', 'GameplayAbilityBlueprint#2220(GA_Fireball)'), NO_COUNT),
    'gas-authoring-06-cost.e2e.json': (
        gas_receipt('SetCost(Class Ability, string Attribute, float Amount, string Effect)',
                    'GA_Fireball_C costs 20 UnrealAIAttributeSet.Mana (/Game/E2EGAS/GA_Fireball_Cost)'),
        gas_receipt('SetCost(Class Ability, string Attribute, float Amount, string Effect)',
                    'GA_Fireball_C costs 20 UnrealAIAttributeSet.Stamina (/Game/E2EGAS/GA_Fireball_Cost)')),
    'gas-authoring-07-cooldown.e2e.json': (
        gas_receipt('SetCooldown(Class Ability, float Seconds, string Tag, string Effect)',
                    'GA_Fireball_C has a 30s cooldown: /Game/E2EGAS/GA_Fireball_Cooldown grants E2E.GAS.Cooldown'),
        gas_receipt('SetCooldown(Class Ability, float Seconds, string Tag, string Effect)',
                    'GA_Fireball_C has a 3s cooldown: /Game/E2EGAS/GA_Fireball_Cooldown grants E2E.GAS.Cooldown')),
    'gas-authoring-08-projectile.e2e.json': (
        gas_receipt('SetAbilityProjectile(Class Ability, Class HitEffect, float Magnitude, Class Projectile, float Speed)',
                    'GA_Fireball_C fires UnrealAIGASProjectile applying GE_Damage_C (SetByCaller.Damage 25)'),
        gas_receipt('SetAbilityProjectile(Class Ability, Class HitEffect, float Magnitude, Class Projectile, float Speed)',
                    'GA_Fireball_C fires UnrealAIGASProjectile applying None (SetByCaller.Damage 0)')),
    'gas-authoring-09-hero.e2e.json': (text_result('Created Blueprint /Game/E2EGAS/BP_Hero (parent Character).'), NO_COUNT),
    'gas-authoring-10-ability-system.e2e.json': (
        gas_receipt('AddAbilitySystem(Blueprint Blueprint, string Attributes, Class AttributeSet)',
                    'UnrealAIAbilitySystemComponent#2301(AbilitySystem)'),
        gas_receipt('AddAbilitySystem(Blueprint Blueprint, string Attributes, Class AttributeSet)', 'null')),
    'gas-authoring-11-regen-on-begin-play.e2e.json': (
        gas_receipt('AddStartingEffect(Blueprint Blueprint, Class Effect)', 'GE_ManaRegen_C'),
        gas_receipt('AddStartingEffect(Blueprint Blueprint, Class Effect)', '')),
    'gas-authoring-12-input.e2e.json': (
        gas_receipt(BIND, 'IA_Fireball (One in IMC_Abilities) activates GA_Fireball_C for the player controlling BP_Hero'),
        gas_receipt(BIND, 'IA_Fireball activates GA_Fireball_C for the player controlling BP_Hero')),
    'gas-authoring-13-display.e2e.json': (
        gas_receipt('AddAttributeDisplay(Blueprint Blueprint, string Format, Vector Offset)',
                    'UnrealAIAttributeTextComponent#2330(AttributeText)'),
        gas_receipt('AddAttributeDisplay(Blueprint Blueprint, string Format, Vector Offset)', 'null')),
    'gas-authoring-14-summary.e2e.json': (
        text_result('1 result\nSummary\n{Cost:GA_Fireball_Cost_C,Cooldown:GA_Fireball_Cooldown_C,CooldownTags:[E2E.GAS.Cooldown]}'),
        text_result('1 result\nSummary\n{Cost:None,Cooldown:None,CooldownTags:[]}')),
    'gas-authoring-15-unknown-attribute.e2e.json': (
        gas_error("AddModifier threw: no attribute 'Manna' (known: UnrealAIAttributeSet.Health, UnrealAIAttributeSet.Mana)"),
        gas_error('AddModifier threw: no effect')),
    'gas-authoring-16-needs-play.e2e.json': (
        gas_error('ActivateAbility threw: the game is not running: start Play In Editor (editor | set IsPlaying = true), then call again'),
        gas_error('ActivateAbility threw: GA_Fireball_C is not granted')),
    'gas-authoring-17-place-hero.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|SpawnActor(string ClassName, string Label)|BP_Hero_C#2400(E2E_Hero)'),
        NO_COUNT),
    'gas-authoring-18-play.e2e.json': (text_result('1 change\n$ref|member|old|new\nEditor@1|IsPlaying|false|true'),
                                       text_result('1 change\n$ref|member|old|new\nEditor@1|IsPlaying|true|false')),
    'gas-authoring-19-pie-cast.e2e.json': (
        gas_receipt(ACTIVATE, 'activated: Mana 100 -> 80'), gas_receipt(ACTIVATE, 'not activated: cannot pay the cost')),
    'gas-authoring-20-pie-cooldown.e2e.json': (
        gas_receipt(ACTIVATE, 'not activated: on cooldown (E2E.GAS.Cooldown, 29.1s left)'),
        gas_receipt(ACTIVATE, 'activated: Mana 80 -> 60')),
    'gas-authoring-21-pie-damage.e2e.json': (
        gas_receipt(APPLY, 'GE_Damage_C applied to E2E_Hero: Health 100 -> 75'),
        gas_receipt(APPLY, 'GE_Damage_C applied to E2E_Hero')),
    'gas-authoring-22-pie-read.e2e.json': (
        text_result('1 result\nAttributes|OwnedTags\n[{Name:Health,Current:75}]|[E2E.GAS.Cooldown]'), text_result('0 results')),
    'gas-authoring-23-stop.e2e.json': (text_result('1 change\n$ref|member|old|new\nEditor@1|IsPlaying|true|false'),
                                       text_result('1 change\n$ref|member|old|new\nEditor@1|IsPlaying|false|true')),
    'gas-authoring-98-cleanup-actor.e2e.json': (
        text_result('1 result\n$ref|method|result\nBP_Hero_C#2400|Destroy()|null'), text_result('0 results')),
    'gas-authoring-99-cleanup.e2e.json': (
        text_result('8 results\n$ref|method|result\nEditor@1|DeleteAsset(string Path, bool bForce)|null'), NO_COUNT),
}
AUTHORING.update(GAS_AUTHORING)


def rig_receipt(method, result):
    return text_result(f'1 result\n$ref|method|result\nControlRigOperations#5120|{method}|{result}')


CREATE_RIG = 'CreateRig(string Path, string Mesh)'
LOOK_AT = 'AddLookAt(ControlRigBlueprint Rig, string Bone, string Target, string Weight, string Axis, string After)'
ADD_VARIABLE = 'AddVariable(ControlRigBlueprint Rig, string Name, string Type, string Default, bool bPublic)'
SET_VARIABLE = 'AddSetVariable(ControlRigBlueprint Rig, string Variable, string Value, string After, string Name)'
WIRE = 'AddToAnimBlueprint(AnimBlueprint AnimBlueprint, ControlRigBlueprint Rig, string After, bool bBindVariables)'
TRACK = ('TrackNearestActor(AnimBlueprint AnimBlueprint, string ActorClass, string LocationVariable, string WeightVariable, '
         'float Radius, float Speed)')
DELETED = text_result('1 result\n$ref|method|result\nEditor@1|DeleteAsset(string Path, bool bForce)|null')
# The Control Rig head look-at sequence: per check, an answer the live UE 5.7 editor gives and one that must not pass.
CONTROL_RIG = {
    'control-rig-units-query.e2e.json': (
        text_result('1 result\nDisplayName|Execute\nAim|true'), text_result('1 result\nDisplayName|Execute\nAim Bone|false')),
    'control-rig-lookat-1-create.e2e.json': (
        rig_receipt(CREATE_RIG, 'ControlRigBlueprint#1297(CR_HeadLookAt)'), rig_receipt(CREATE_RIG, 'ControlRigBlueprint#1297(CR_HeadLookAt2)')),
    'control-rig-lookat-2-target.e2e.json': (rig_receipt(ADD_VARIABLE, 'LookAtTarget'), rig_receipt(ADD_VARIABLE, 'LookAtTarget_1')),
    'control-rig-lookat-3-weight.e2e.json': (rig_receipt(ADD_VARIABLE, 'LookAtWeight'), rig_receipt(ADD_VARIABLE, 'LookAtWeight_1')),
    'control-rig-lookat-4-aim.e2e.json': (
        rig_receipt(LOOK_AT, 'LookAt_head'), rig_receipt(LOOK_AT, 'LookAt_head_1')),
    'control-rig-lookat-5-anim-blueprint.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|CopyAsset(string Source, string Destination)|AnimBlueprint#88(ABP_HeadLookAt)'),
        NO_COUNT),
    'control-rig-lookat-6-wire.e2e.json': (
        rig_receipt(WIRE, 'AnimGraphNode_ControlRig_0 runs CR_HeadLookAt between Default Slot and Output Pose; inputs: '
                          'LookAtTarget <- LookAtTarget (new Vector variable), LookAtWeight <- LookAtWeight (new Float variable)'),
        rig_receipt(WIRE, 'AnimGraphNode_ControlRig_0 runs CR_HeadLookAt between Default Slot and Output Pose')),
    'control-rig-lookat-7-track.e2e.json': (
        rig_receipt(TRACK, 'Event Blueprint Update Animation: LookAtTarget (existing variable) follows the nearest StaticMeshActor '
                           'within 600 cm of the owner (smoothed at speed 4); LookAtWeight (existing variable) blends to 1'),
        rig_receipt(TRACK, 'Event Blueprint Update Animation: LookAtTarget (new Vector variable) follows the nearest '
                           'StaticMeshActor within 600 cm of the owner')),
    'control-rig-lookat-8-compile.e2e.json': (
        text_result('1 result\n$ref|method|result\nAnimBlueprint#88|Compile()|{Succeeded:true,Errors:0,Warnings:0,Messages:[]}'),
        text_result('1 result\n$ref|method|result\nAnimBlueprint#88|Compile()|{Succeeded:false,Errors:1,Warnings:0,Messages:[x]}')),
    'control-rig-lookat-8-readback.e2e.json': (
        text_result('1 result\nControlRigs\n[{Node:AnimGraphNode_ControlRig_0,Rig:CR_HeadLookAt,Graph:AnimGraph,After:x,'
                    'Before:Output Pose,Inputs:{LookAtTarget:@LookAtTarget,LookAtWeight:@LookAtWeight}}]'),
        text_result('1 result\nControlRigs\n[{Node:AnimGraphNode_ControlRig_0,Rig:CR_HeadLookAt,Graph:AnimGraph,After:x,'
                    'Before:Output Pose,Inputs:{LookAtTarget:(X=0,Y=0,Z=0)}}]')),
    'control-rig-lookat-8-unknown-unit.e2e.json': (
        '{"error": "error call-threw: AddUnit threw: no rig unit named \'Aimm\'; similar: AimItem (Aim)"}',
        '{"error": "error call-threw: AddUnit threw: CR_HeadLookAt has no rig graph"}'),
    'control-rig-lookat-9-cleanup-abp.e2e.json': (DELETED, NO_COUNT),
    'control-rig-lookat-9-cleanup-rig.e2e.json': (DELETED, NO_COUNT),
    'control-rig-trace-00-missing-asset.e2e.json': (
        text_result("warning asset-not-found: no asset at '/Game/E2E/NoSuchRigAsset': unreal.asset(path) is empty\n0 results\nNodes"),
        text_result('0 results\nNodes')),
    'control-rig-trace-01-create.e2e.json': (
        rig_receipt(CREATE_RIG, 'ControlRigBlueprint#1297(CR_RigTrace)'), rig_receipt(CREATE_RIG, 'ControlRigBlueprint#1297(CR_RigTrace2)')),
    'control-rig-trace-02-hit-variable.e2e.json': (rig_receipt(ADD_VARIABLE, 'Hit'), rig_receipt(ADD_VARIABLE, 'Hit_1')),
    'control-rig-trace-03-height-variable.e2e.json': (rig_receipt(ADD_VARIABLE, 'HitZ'), rig_receipt(ADD_VARIABLE, 'HitZ_1')),
    'control-rig-trace-04-trace.e2e.json': (
        rig_receipt('AddUnit(ControlRigBlueprint Rig, string Unit, string Settings, string After, string Name)', 'Trace'),
        rig_receipt('AddUnit(ControlRigBlueprint Rig, string Unit, string Settings, string After, string Name)', 'Trace_1')),
    'control-rig-trace-05-store-hit.e2e.json': (rig_receipt(SET_VARIABLE, 'VariableNode'), NO_COUNT),
    'control-rig-trace-06-store-height.e2e.json': (rig_receipt(SET_VARIABLE, 'VariableNode_1'), NO_COUNT),
    'control-rig-trace-07-curve-refused.e2e.json': (
        '{"error": "error call-threw: AddUnit threw: CR_RigTrace has no curve \'DebugHit\', so this node would silently do nothing. AddVariable, AddSetVariable, then RigValues"}',
        '{"error": "error call-threw: AddUnit threw: no rig unit named SetCurveValue"}'),
    'control-rig-trace-08-needs-play.e2e.json': (
        '{"error": "error call-threw: RigValues threw: no Play In Editor session: a rig runs only while playing"}',
        '{"error": "error call-threw: RigValues threw: no Control Rig runs in the play world"}'),
    'control-rig-trace-09-anim-blueprint.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|CopyAsset(string Source, string Destination)|AnimBlueprint#88(ABP_RigTrace)'),
        NO_COUNT),
    'control-rig-trace-10-wire.e2e.json': (
        rig_receipt(WIRE, 'AnimGraphNode_ControlRig_0 runs CR_RigTrace between Default Slot and Output Pose'), NO_COUNT),
    'control-rig-trace-11-compile.e2e.json': (
        text_result('1 result\n$ref|method|result\nAnimBlueprint#88|Compile()|{Succeeded:true,Errors:0,Warnings:0,Messages:[]}'),
        text_result('1 result\n$ref|method|result\nAnimBlueprint#88|Compile()|{Succeeded:false,Errors:1,Warnings:0,Messages:[x]}')),
    'control-rig-trace-12-floor.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|SpawnActorFromAsset(...)|StaticMeshActor#9(E2E_RigTraceFloor)'), NO_COUNT),
    'control-rig-trace-13-mesh.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|SpawnActor(...)|SkeletalMeshActor#9(E2E_RigTraceMesh)'), NO_COUNT),
    'control-rig-trace-14-mesh-setup.e2e.json': (
        text_result('1 result\n$ref|member|old|new\nSkeletalMeshComponent#9|AnimClass|null|ABP_RigTrace_C'), NO_COUNT),
    'control-rig-trace-15-play.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|StartPlay(bool bPaused)|{Playing:true,Paused:true}'),
        text_result('1 result\n$ref|method|result\nEditor@1|StartPlay(bool bPaused)|{Playing:false,Paused:true}')),
    'control-rig-trace-16-advance.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|AdvancePlay(...)|{Playing:true,FramesAdvanced:30}'),
        text_result('1 result\n$ref|method|result\nEditor@1|AdvancePlay(...)|{Playing:true,FramesAdvanced:0}')),
    'control-rig-trace-17-read.e2e.json': (
        text_result('1 result\n$ref|method|result\nControlRigOperations#1|RigValues(string Rig)|[{Rig:CR_RigTrace,RigOrigin:"(5000, 0, 100)",Executions:30,Variables:{Hit:True,HitZ:"-100.000000"},Notes:[]}]'),
        text_result('1 result\n$ref|method|result\nControlRigOperations#1|RigValues(string Rig)|[{Rig:CR_RigTrace,RigOrigin:"(5000, 0, 100)",Executions:30,Variables:{Hit:False,HitZ:"0.000000"},Notes:[]}]')),
    'control-rig-trace-18-stop.e2e.json': (
        text_result('1 result\n$ref|method|result\nEditor@1|StopPlay()|null'), NO_COUNT),
    'control-rig-trace-19-cleanup-actors.e2e.json': (
        text_result('2 results\n$ref|method|result\nStaticMeshActor#9|Destroy()|null'), NO_COUNT),
    'control-rig-trace-20-cleanup-abp.e2e.json': (DELETED, NO_COUNT),
    'control-rig-trace-21-cleanup-rig.e2e.json': (DELETED, NO_COUNT),
}
AUTHORING.update(CONTROL_RIG)


SCULPT = 'LandscapeOperations#9190|Sculpt(LandscapeProxy Landscape, Vector Center, double Radius, double Height)'
WATER = 'WaterOperations#9170'
# The suite editor runs -nullrhi: water bodies report Ready false there (no water mesh without rendering).
WATER_STATUS = ('2 results\nLabel|Water|WaterZone.ZoneExtent\nE2EOcean|{{Type:Ocean,SplinePoints:4,Material:Water_Material_Ocean,'
                'AffectsLandscape:false,Ready:false,Problem:"this editor does not render (-nullrhi): the water mesh is built in a rendering '
                'editor"}}|(250000, 250000)\nE2ERiver|{{Type:River,SplinePoints:36,Material:Water_Material_River,AffectsLandscape:{},'
                'Ready:false,Problem:""}}|(250000, 250000)')
# The Landscape island + water sequence: per check, an answer the live UE 5.7 editor gives and one that must not pass.
AUTHORING.update({
    'water-1-island-landscape.e2e.json': (
        pcg_receipt('LandscapeOperations#9190|Create(string Label, Vector Location, double SizeMeters, MaterialInterface Material)'
                    '|Landscape#9191(E2EIsland)'), NO_COUNT),
    'water-2-island-sea-floor.e2e.json': (pcg_receipt(f'{SCULPT}|3196021'), pcg_receipt(f'{SCULPT}|0')),
    'water-3-island-land.e2e.json': (pcg_receipt(f'{SCULPT}|1803641'), pcg_receipt(f'{SCULPT}|0')),
    'water-4-island-mountain.e2e.json': (pcg_receipt(f'{SCULPT}|290393'), pcg_receipt(f'{SCULPT}|0')),
    'water-5-zone.e2e.json': (
        pcg_receipt(f'{WATER}|SetZone(double SizeMeters, Vector Center)|WaterZone#9275(WaterZone)'),
        {'content': [{'type': 'text', 'text': 'error call-threw: SetZone threw: SizeMeters 2500 is outside'}], 'isError': True}),
    'water-6-ocean.e2e.json': (
        pcg_receipt(f'{WATER}|CreateOcean(string Label, double Height, MaterialInterface Material)|WaterBodyOcean#9280(E2EOcean)'),
        {'content': [{'type': 'text', 'text': 'error call-threw: CreateOcean threw: the level already has an ocean'}], 'isError': True}),
    'water-7-river.e2e.json': (
        pcg_receipt(f'{WATER}|CreateRiverBetween(string Label, LandscapeProxy Landscape, Vector Source, Vector Mouth, double Width, '
                    'double Depth, MaterialInterface Material)|WaterBodyRiver#9304(E2ERiver)'), NO_COUNT),
    'water-8-status.e2e.json': (text_result(WATER_STATUS.format('false')), text_result(WATER_STATUS.format('true'))),
    'water-9-cleanup.e2e.json': (
        text_result('4 results\nmethod: Destroy()\n$ref|result\nLandscape#9191|null\nWaterZone#9275|null\nWaterBodyOcean#9280|null\n'
                    'WaterBodyRiver#9304|null'),
        text_result('1 result\nmethod: Destroy()\n$ref|result\nLandscape#9191|null')),
})

class CheckFileTests(unittest.TestCase):
    def test_every_plugin_has_valid_checks_and_no_powershell_left(self):
        for plugin, folder in release.components().items():
            self.assertTrue(e2e.checks_for(plugin), plugin)
            self.assertEqual(list((folder / 'Tests/e2e/tools').glob('*.ps1')), [], plugin)
        self.assertEqual(len(all_checks()), 143)

    def test_every_assert_matches_its_intended_shape_and_rejects_others(self):
        for (plugin, name), check in all_checks().items():
            with self.subTest(check=f'{plugin}/{name}'):
                pattern = check['assert']['pattern']
                if 'count' in check['input'].get('query', ''):
                    # A count answer arrives as the text of a content item, a structured value, or a value in text JSON.
                    for good in (COUNT, STRUCTURED_COUNT, ESCAPED_COUNT):
                        self.assertTrue(matches(check, good), good)
                    self.assertFalse(matches(check, NO_COUNT))
                    self.assertFalse(matches(check, text_result('value: none')))
                elif name in AUTHORING or name in SCHEME:
                    good, bad = AUTHORING[name] if name in AUTHORING else SCHEME[name]
                    code = 1 if check.get('expectError') else 0
                    self.assertTrue(evaluates(check, code, good), good)
                    self.assertFalse(evaluates(check, code, bad), bad)
                elif check['tool'] == 'rql-schema':
                    # The schema lists the expected member/operation by name.
                    self.assertTrue(matches(check, text_result(f'Members: Name, {pattern}(Target)')))
                    self.assertFalse(matches(check, NO_COUNT))
                else:
                    # inputContexts rows: PowerShell -match is case-insensitive, so any "result" text passes.
                    self.assertEqual(pattern, 'result')
                    self.assertTrue(matches(check, {'structuredContent': {'result': {'kind': 'rows', 'rows': []}}}))
                    self.assertFalse(matches(check, NO_COUNT))

    def test_a_type_filter_count_accepts_rqls_no_match_warning_for_its_type_only(self):
        # RQL 0.3.0: an empty level answers `where $ is <Type> | count` with its no-match warning, the count on the last
        # line. The extension spec proves a placed component matches; the e2e only needs the answer's shape.
        filtered = {key: check for key, check in all_checks().items() if '| where $ is ' in check['input'].get('query', '')}
        self.assertEqual(sorted(plugin for plugin, _ in filtered), ['UnrealAIControlRig', 'UnrealAIPCG'])
        for (plugin, name), check in filtered.items():
            type_name = check['input']['query'].split('| where $ is ')[1].split()[0]
            warning = (f"warning no-match: none of the 730 tested item(s) is a {type_name} (saw BrushComponent) \u2014 "
                       f"hint: no reachable type is named '{type_name}'; list the types with rql-schema")
            with self.subTest(check=f'{plugin}/{name}'):
                self.assertTrue(matches(check, text_result(warning + '\n0')))
                self.assertFalse(matches(check, text_result(warning)), 'a warning with no count')
                self.assertFalse(matches(check, text_result(warning.replace(type_name, 'FooComponent') + '\n0')),
                                 "another type's warning")
                self.assertFalse(matches(check, text_result(warning.replace(type_name, type_name + 'X') + '\n0')),
                                 'a longer type name')
                self.assertFalse(matches(check, text_result(warning + '\nwarning getter-threw: x\n0')),
                                 'the warning covers one line only')

    def test_schema_rejects_malformed_checks(self):
        bad = [
            {'tool': 'rql-query'},
            {'description': 'x', 'tool': 'Bad_Tool'},
            {'description': 'x', 'tool': 'rql-query', 'input': '{"query":"a"}'},
            {'description': 'x', 'tool': 'rql-query', 'system': 'no'},
            {'description': 'x', 'tool': 'rql-query', 'expectError': True},
            {'description': 'x', 'tool': 'rql-query', 'assert': {'pattern': '('}},
            {'description': 'x', 'tool': 'rql-query', 'assert': {'notPattern': 'a'}},
            {'description': 'x', 'tool': 'rql-query', 'Assert': {'pattern': 'a'}},
        ]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'bad.e2e.json'
            for check in bad:
                path.write_text(json.dumps(check), encoding='utf-8')
                with self.subTest(check=check), self.assertRaises(ValueError):
                    e2e.load_check(path)

    def test_serialization_is_compact_and_keeps_non_ascii(self):
        self.assertEqual(e2e.serialize({'a': [1, 'é']}), '{"a":[1,"é"]}')


class RunnerTests(unittest.TestCase):
    CHECK = {'description': 'the count', 'tool': 'rql-query', 'input': {'query': 'a | count'},
             'assert': {'pattern': r'"text"\s*:\s*"\d+"'}}

    def run_with(self, checks, outputs):
        """Run plugin 'P' with the given checks against a fake CLI returning (code, text) per call."""
        calls = []

        def fake_run(args, **kwargs):
            calls.append(args)
            code, text = outputs[len(calls) - 1]
            return subprocess.CompletedProcess(args, code, stdout=text)

        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / 'extensions/P/Tests/e2e/tools'
            folder.mkdir(parents=True)
            for index, check in enumerate(checks):
                (folder / f'c{index}.e2e.json').write_text(json.dumps(check), encoding='utf-8')
            with patch.object(e2e, 'ROOT', Path(temporary)), patch.object(e2e.subprocess, 'run', fake_run), \
                    redirect_stdout(io.StringIO()):
                failures = e2e.run('/proj', 'http://localhost:1', ['P'])
        return failures, calls

    def test_success_builds_the_cli_call(self):
        failures, calls = self.run_with([self.CHECK, dict(self.CHECK, system=True, input={})],
                                        [(0, json.dumps(COUNT)), (0, json.dumps(COUNT))])
        self.assertEqual(failures, [])
        self.assertEqual(calls[0][2:], ['run-tool', 'rql-query', '--path', '/proj', '--url', 'http://localhost:1',
                                        '--input', '{"query":"a | count"}'])
        self.assertEqual(calls[1][2], 'run-system-tool')
        self.assertEqual(calls[1][-2:], ['--input', '{}'])

    def test_missing_checks_fail(self):
        failures, calls = self.run_with([], [])
        self.assertEqual(calls, [])
        self.assertIn('Missing E2E checks', failures[0])

    def test_failure_rules(self):
        cases = [
            ((1, 'ERROR: boom'), 'CLI failed'),
            ((0, 'ECONNREFUSED 127.0.0.1'), 'Transport failure'),
            ((0, 'not json'), 'Invalid success'),
            ((0, '[1]'), 'Invalid success'),
            ((0, json.dumps(dict(COUNT, isError=True))), 'Invalid success'),
            ((0, json.dumps(NO_COUNT)), 'expected the count'),
        ]
        for output, message in cases:
            with self.subTest(output=output):
                failures, _ = self.run_with([self.CHECK], [output])
                self.assertEqual(len(failures), 1)
                self.assertIn('P/rql-query: ' + message, failures[0])

    def test_expected_error_rules(self):
        check = {'description': 'the not-found error', 'tool': 'rql-query', 'expectError': True,
                 'assert': {'pattern': 'unknown source'}}
        failure = 'ERROR: Failed to run tool "rql-query" (http): Unknown source "x"'
        self.assertEqual(self.run_with([check], [(1, failure)])[0], [])
        self.assertIn('expected the not-found error', self.run_with([check], [(1, 'ERROR: other')])[0][0])
        transport = 'ERROR: Failed to run tool "rql-query" (network): unknown source'
        self.assertIn('Transport failure', self.run_with([check], [(1, transport)])[0][0])
        missing_assert = {'description': 'x', 'tool': 'rql-query', 'expectError': True}
        self.assertIn('must assert', self.run_with([missing_assert], [])[0][0])

    def test_one_failure_does_not_stop_later_checks(self):
        failures, calls = self.run_with([self.CHECK, self.CHECK], [(1, 'ERROR: boom'), (0, json.dumps(COUNT))])
        self.assertEqual(len(calls), 2)
        self.assertEqual(len(failures), 1)


if __name__ == '__main__':
    unittest.main()
