// `create-project` — scaffold a minimal Unreal Engine C++ project from
// in-CLI templates (no engine binary required — UBT compiles it later via
// `open` / a build). Mirrors the minimal layout of the infra
// `Unreal-Test-Project` testbed: a `.uproject`, one runtime module, Game +
// Editor targets, and minimal `Config/`.
//
// Library-safe: never prints, never exits, never throws past the boundary.

import * as fs from 'fs';
import * as path from 'path';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { CreateProjectOptions, CreateProjectResult } from './types.js';

const VALID_MODULE_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;

/**
 * Validate a UE module/project name. UE module names cannot contain
 * hyphens or spaces and must be a valid C++ identifier. Pure.
 */
export function validateModuleName(name: string): { ok: true } | { ok: false; reason: string } {
  if (name.length === 0) return { ok: false, reason: 'name is empty' };
  if (!VALID_MODULE_NAME.test(name)) {
    return {
      ok: false,
      reason: `"${name}" is not a valid UE module name (letters, digits, underscore; no leading digit, no hyphen/space)`,
    };
  }
  return { ok: true };
}

interface TemplateFile {
  relPath: string;
  content: string;
}

/**
 * Render the full set of scaffold files for a project. Pure — returns the
 * relative paths + contents without touching disk, so tests can assert the
 * template output directly. Exported for tests.
 */
export function renderProjectTemplate(name: string, engineAssociation: string): TemplateFile[] {
  const uproject = {
    FileVersion: 3,
    EngineAssociation: engineAssociation,
    Category: '',
    Description: '',
    Modules: [{ Name: name, Type: 'Runtime', LoadingPhase: 'Default' }],
  };

  const buildCs = `// Copyright (c) 2026 Ivan Murzak (Apache-2.0)
using UnrealBuildTool;

public class ${name} : ModuleRules
{
\tpublic ${name}(ReadOnlyTargetRules Target) : base(Target)
\t{
\t\tPCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
\t\tPublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });
\t}
}
`;

  const moduleHeader = `// Copyright (c) 2026 Ivan Murzak (Apache-2.0)
#pragma once

#include "CoreMinimal.h"
`;

  const moduleCpp = `// Copyright (c) 2026 Ivan Murzak (Apache-2.0)
#include "${name}.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ${name}, "${name}");
`;

  const gameTarget = `// Copyright (c) 2026 Ivan Murzak (Apache-2.0)
using UnrealBuildTool;

public class ${name}Target : TargetRules
{
\tpublic ${name}Target(TargetInfo Target) : base(Target)
\t{
\t\tType = TargetType.Game;
\t\tDefaultBuildSettings = BuildSettingsVersion.Latest;
\t\tIncludeOrderVersion = EngineIncludeOrderVersion.Latest;
\t\tExtraModuleNames.Add("${name}");
\t}
}
`;

  const editorTarget = `// Copyright (c) 2026 Ivan Murzak (Apache-2.0)
using UnrealBuildTool;

public class ${name}EditorTarget : TargetRules
{
\tpublic ${name}EditorTarget(TargetInfo Target) : base(Target)
\t{
\t\tType = TargetType.Editor;
\t\tDefaultBuildSettings = BuildSettingsVersion.Latest;
\t\tIncludeOrderVersion = EngineIncludeOrderVersion.Latest;
\t\tExtraModuleNames.Add("${name}");
\t}
}
`;

  const defaultEngine = `[/Script/EngineSettings.GameMapsSettings]
GameDefaultMap=/Engine/Maps/Templates/OpenWorld

[/Script/Engine.RendererSettings]
r.GenerateMeshDistanceFields=True
`;

  const defaultGame = `[/Script/EngineSettings.GeneralProjectSettings]
ProjectID=${generateProjectId()}
ProjectName=${name}
`;

  const defaultEditor = `[/Script/UnrealEd.EditorPerProjectUserSettings]
`;

  const gitignore = `# Unreal Engine generated
Binaries/
Intermediate/
Saved/
DerivedDataCache/
.vs/
*.sln
# Unreal-MCP local secrets — never commit
.env
`;

  return [
    { relPath: `${name}.uproject`, content: JSON.stringify(uproject, null, '\t') + '\n' },
    { relPath: path.join('Source', name, `${name}.Build.cs`), content: buildCs },
    { relPath: path.join('Source', name, `${name}.h`), content: moduleHeader },
    { relPath: path.join('Source', name, `${name}.cpp`), content: moduleCpp },
    { relPath: path.join('Source', `${name}.Target.cs`), content: gameTarget },
    { relPath: path.join('Source', `${name}Editor.Target.cs`), content: editorTarget },
    { relPath: path.join('Config', 'DefaultEngine.ini'), content: defaultEngine },
    { relPath: path.join('Config', 'DefaultGame.ini'), content: defaultGame },
    { relPath: path.join('Config', 'DefaultEditor.ini'), content: defaultEditor },
    { relPath: '.gitignore', content: gitignore },
  ];
}

function generateProjectId(): string {
  // A UE ProjectID is a braced uppercase GUID. crypto.randomUUID is fine.
  const uuid =
    typeof globalThis.crypto?.randomUUID === 'function'
      ? globalThis.crypto.randomUUID()
      : '00000000-0000-0000-0000-000000000000';
  return `{${uuid.toUpperCase()}}`;
}

export async function createProject(opts: CreateProjectOptions): Promise<CreateProjectResult> {
  const warnings: string[] = [];
  let projectDir: string | undefined;
  try {
    if (!opts || typeof opts.projectDir !== 'string' || opts.projectDir.trim().length === 0) {
      throw new Error('projectDir is required.');
    }
    projectDir = path.resolve(opts.projectDir);
    const projectName = (opts.projectName ?? path.basename(projectDir)).trim();

    const nameCheck = validateModuleName(projectName);
    if (!nameCheck.ok) {
      throw new Error(
        `Invalid project name: ${nameCheck.reason}. ` +
          'Pass --name with a valid UE module name (UE module names cannot contain hyphens).',
      );
    }

    if (fs.existsSync(projectDir)) {
      const entries = fs.readdirSync(projectDir);
      if (entries.length > 0 && !opts.force) {
        throw new Error(
          `Target directory is not empty: ${projectDir}. Pass --force to scaffold anyway.`,
        );
      }
    }

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Scaffolding Unreal project ${projectName} at ${projectDir}`,
    });

    const engineAssociation = opts.engineAssociation?.trim() ?? '';
    const files = renderProjectTemplate(projectName, engineAssociation);

    const filesWritten: string[] = [];
    for (const file of files) {
      const abs = path.join(projectDir, file.relPath);
      fs.mkdirSync(path.dirname(abs), { recursive: true });
      fs.writeFileSync(abs, file.content, 'utf-8');
      filesWritten.push(abs);
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Wrote ${file.relPath}`,
        filePath: abs,
      });
    }

    const uprojectPath = path.join(projectDir, `${projectName}.uproject`);
    emitProgress(opts.onProgress, { phase: 'done', message: 'Project scaffolded.' });

    return {
      kind: 'success',
      success: true,
      projectDir,
      projectName,
      uprojectPath,
      filesWritten,
      warnings,
    };
  } catch (err: unknown) {
    const error = asError(err);
    return {
      kind: 'failure',
      success: false,
      projectDir,
      warnings,
      errorMessage: error.message,
      error,
    };
  }
}
