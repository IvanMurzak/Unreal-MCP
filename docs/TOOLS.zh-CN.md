<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

# Unreal-MCP 工具

这是 **UnrealMCP** 编辑器插件所注册工具的权威清单。Unreal-MCP 提供 **按类别分组的内置工具**供你的 AI 智能体调用，另有 **系统工具**是它不能调用的（供 CLI 和桌面应用使用的宿主底层设施）。工具 id 使用 kebab-case（`rql-query`、`blueprint-compile`），与 Unity-MCP 和 Godot-MCP 的约定相同。[扩展](#扩展)和[你自己的插件](EXTENSIONS.md)可以添加更多工具。

| 类别 | 工具 |
| --- | --- |
| RQL 数据层 | `rql-query`、`rql-mutate`、`rql-schema` |
| Blueprint | `blueprint-*`：create、get、add-component、remove-component、add-variable、modify-variable、set-default、add-function、add-event、compile、spawn |
| 源码 / C++ | `source-*`：read、create-class、update、delete、list、compile |
| 截图 | `screenshot-*`：viewport、game-view、camera、isolated |
| Fab | `fab-search`、`fab-library`、`fab-add` |
| 控制台 | `console-run-command` |
| Python | `python-execute` |
| 工具列表 | `unreal-tool-list` |
| **系统**（不向 AI 展示） | `ping`、`unreal-skill-create`、`unreal-skill-generate`、`editor-quit` |

> **权威来源。** 标准工具：`UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` 中的 `UnrealMcp*Tools.cpp`（`UnrealMcpSkillTools.cpp` 除外）以及 `UnrealMcpRql` 中的 `UnrealMcpRqlTools.cpp`。系统工具：`UnrealMcpPingTool.cpp`、`UnrealMcpSkillTools.cpp`、`UnrealMcpEditorSession.cpp` 以及边车进程的 `SkillGenerateTool.cs`。注册发生变化时请更新本页。

全部标准工具都**仅限编辑器**。运行时（游戏内）连接只提供 `ping` 以及你的游戏自己注册的工具：见 [RUNTIME.md](RUNTIME.zh-CN.md)。

## 各类别详情

<details>
  <summary><b>RQL — 数据层</b></summary>

用一种查询语言覆盖整个编辑器，而不是每个操作一个工具（[`docs/rql.md`](rql.md)）：在一次调用中查找 Actor、组件和资源，读取成员，过滤、聚合并修改它们。它取代了以前的 actor、object、asset、level、editor-state、selection、console-log 和 reflection 工具。

| 工具 id | 作用 |
| --- | --- |
| `rql-query` | 对实时编辑器执行只读 RQL 查询（`actors | where $ is PointLight | select $ref Label Intensity`）；返回紧凑的表格结果；一次调用可执行以 `;` 分隔的多条查询 |
| `rql-mutate` | 以 `set` / `call` 结尾的 RQL 查询（`... | set Location = "(0, 0, 100)"`、`editor | call SpawnActor("PointLight")`）：全部成功或全部不执行，一个撤销步骤，`dry_run` 可预览；以 `;` 分隔的多条查询各自是一次更改 |
| `rql-schema` | 数据源、函数以及编辑器世界中的类；`type` 列出某个类型的成员和方法 |

除了反射属性，RQL 还能访问 DataTable 行（`dataTables`、`Rows["Sword"].Damage`）、文本形式的 gameplay tag（`gameplayTags`、`set Tags = ["Ability.Fire"]`）、保存到 ini 的 Project Settings（`unreal.settings("Maps & Modes")`）、Blueprint 图表（`EventGraph | call AddCallFunction("PrintString")`、引脚连接、`call Compile()`）、Widget Blueprint 树（`call AddWidget("Button", "Ok", parent)`）以及 Level Sequence（绑定、轨道、`call AddKey("Location.X", 30, 100)`）。

`unreal-rql` skill（由 `unreal-skill-generate` 写出）讲解这门语言，并列出已安装的扩展添加了什么。

</details>

<details>
  <summary><b>Blueprint 类别— Unreal 的核心能力</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `blueprint-create` | 从父 `UClass` 路径创建新的 Blueprint 类 |
| `blueprint-get` | 供 LLM 检查的图表摘要（变量、组件、函数/事件、父类链） |
| `blueprint-add-component` | 通过 Simple Construction Script 添加组件 |
| `blueprint-remove-component` | 移除一个 SCS 组件 |
| `blueprint-add-variable` | 添加一个有类型的成员变量 |
| `blueprint-modify-variable` | 修改一个成员变量 |
| `blueprint-set-default` | 编辑 CDO（类默认值）属性 |
| `blueprint-add-function` | 添加函数存根（已连好入口/结果节点） |
| `blueprint-add-event` | 添加/绑定事件存根（BeginPlay、Tick、输入……） |
| `blueprint-compile` | 编译 Blueprint 并返回**结构化的错误/警告列表**（AI 的反馈循环） |
| `blueprint-spawn` | 把 Blueprint 实例化到当前关卡 |

</details>

<details>
  <summary><b>控制台类别</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `console-run-command` | 运行控制台命令 / CVar |

日志、PIE 状态和选择都通过 RQL 处理：`console | where Verbosity == "Error"`、`editor | set IsPlaying = true`、`editor | set Selection = [...]`。试玩测试在调用内部模拟游戏（结果确定，不受编辑器在两次调用之间 tick 的影响）：`editor | call StartPlay()`、`editor | call AdvancePlay(1.5, 0, "W, LeftShift")`（按住按键 1.5 秒，返回 pawn 的位置、速度、本次调用使它移动的距离、移动模式和已应用的映射上下文）、`editor | call StopPlay()`；Enhanced Input 扩展还添加了 `enhancedInput | call InjectInput(action, value, seconds)`（[docs/rql.md](rql.md#play-testing)）。

</details>

<details>
  <summary><b>工具列表类别</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `unreal-tool-list` | 列出所有工具，包括在 `tools/list` 中隐藏的工具（标记为 hidden；按名称调用时仍会运行）；支持正则 `filter`、`includeDescription`、`includeInputs` |

</details>

<details>
  <summary><b>Python 类别</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `python-execute` | 在编辑器中运行 Python（完整的 `unreal` API，已预先导入）：普通语句，最后一个表达式即为结果，`print` 作为日志返回，错误指向 `snippet.py` 的行号；`include` 从 `<Project>/.ai-game-dev/snippets` 添加辅助文件/文件夹；`timeout`（秒，默认 30）会停止失控的代码片段 |

使用引擎的 PythonScriptPlugin（由 Unreal-MCP 启用；仅限编辑器，绝不会进入打包的游戏）以及内置在 `UnrealMCP/Content/Python` 中的 [Code-Exec](https://github.com/IvanMurzak/Code-Exec) 运行时。Blueprint 图表连线不向 Python 开放：请使用 Blueprint 工具。

</details>

<details>
  <summary><b>Fab 类别— 搜索并添加 Fab 内容</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `fab-search` | 搜索 [Fab](https://www.fab.com) 市场（`query`、`listingType`、`freeOnly`、`sort`、分页）；返回每个商品的 uid、标题、类型、价格和格式 |
| `fab-library` | 列出已登录用户拥有的 Fab 资源，以及每个资源已导入到的位置；`filter` 搜索整个库 |
| `fab-add` | 下载用户拥有的商品，并像 Fab 面板的 *Add to Project* 一样导入（Unreal Engine 资源包、带 `quality` 的 Megascans、glTF/FBX）；返回内容文件夹，跳过项目中已有的商品 |

它们驱动引擎自带的 Fab 插件（自 UE 5.5 起默认启用）：使用它的 Epic 登录和导入流程。未登录时，首次调用 `fab-library` / `fab-add` 会在浏览器中打开 Epic 的登录页面（从编辑器工具栏打开 Fab 也会登录，使用记住的 Epic 登录信息）。购买商品，或把免费商品添加到库中，仍需在 fab.com 上完成。完整项目和引擎代码插件通过 Epic Games Launcher 安装，而不是添加到项目中。

</details>

<details>
  <summary><b>源码 / C++ 类别</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `source-read` | 读取项目的 C++ 源文件（分片） |
| `source-create-class` | 生成新的 C++ 类骨架（根据模板生成头文件 + cpp） |
| `source-update` | 编辑源文件 |
| `source-delete` | 删除源文件 |
| `source-list` | 列出模块源文件 |
| `source-compile` | 编译项目 C++（Live Coding 处于活动状态时使用它，否则使用 UBT）并给出结构化错误报告；对于新类或已更改的反射声明（`restartRequired`），会说明如何重新构建并重启编辑器 |

所有文件操作都限制在 `<Project>/Source/` 内。

</details>

<details>
  <summary><b>截图 / 视口捕获类别</b></summary>

| 工具 id | 作用 |
| --- | --- |
| `screenshot-viewport` | 捕获当前活动的编辑器视口；可选的 `savePath` 还会把 PNG 写入项目内 |
| `screenshot-game-view` | 捕获 PIE / 游戏视图；可选的 `savePath` 同上 |
| `screenshot-camera` | 通过 `USceneCaptureComponent2D` 从解析出的相机 Actor 渲染；可选的 `savePath` 同上 |
| `screenshot-isolated` | 单独渲染一个 Actor（临时 SceneCapture2D + 仅显示列表）；可选的 `savePath` 同上 |

捕获结果以 base64 **PNG 作为 MCP 图像内容**返回，LLM 可以直接查看渲染画面。每次捕获都在调用时渲染新的一帧，即使编辑器窗口在后台或已最小化也是如此，返回的文本会说明该帧是否为新帧，以及特效和动画是否在 tick。AI 智能体连接期间，插件会让编辑器在后台（包括最小化时）以全速保持 tick 和渲染（"Use Less CPU when in Background" 仅在该会话中被覆盖；你保存的偏好设置不变）。尺寸会被限制（默认 1024，每边上限 2048）。像素捕获需要一个会渲染的编辑器：窗口模式，或带 `-RenderOffscreen` 的无界面模式（`unreal-mcp-cli` 的默认设置）。在 `-nullrhi` 下不会渲染任何内容，因此这些工具会返回一个指明 `-nullrhi` 及修复方法的错误。

</details>

<details>
  <summary><b>系统工具— 不向你的 AI 开放</b></summary>

这些是**系统工具**：`unreal-mcp-cli` 和桌面应用通过 `POST /api/system-tools/<name>` 直接调用的宿主底层设施。它们被有意排除在 MCP `tools/list` 之外，因此永远不会出现在 AI 会话中（也不会消耗其 token）——Unity 和 Godot 对其系统工具也采用同样的划分。

| 工具 id | 作用 |
| --- | --- |
| `ping` | 存活探测 — 在插件 ⇄ 边车进程 ⇄ 服务器链路上往返一次 |
| `unreal-skill-create` | 在插件的编辑器模块中以 C++ 文件形式生成一个新的 MCP 工具。新工具可调用前**需要重新构建编辑器**（Live Coding，或重新构建项目）——结果会明确说明这一点。需要以源码方式安装插件 |
| `unreal-skill-generate` | 根据已注册的工具，把所有 `SKILL.md` 重新生成到相对于项目的 skills 文件夹（默认 `.claude/skills`） |
| `editor-quit` | 保存有路径的未保存包，并退出由 CLI 会话启动的编辑器（`unreal-mcp-cli close --owned-only`）。除非 `nonce` 与会话租约匹配，否则拒绝执行，因此绝不会关闭你自己打开的编辑器 |

</details>

## 扩展

当你的项目使用下列引擎系统之一时，安装对应的扩展，或[创建你自己的扩展](EXTENSIONS.md)。每个扩展都是一个 RQL 模块：它扩展核心的 `rql-query` / `rql-mutate` / `rql-schema` 工具，并把自己的指南加入 `unreal-rql` skill，因此 `tools/list` 中不会出现单独的 MCP 工具。

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

扩展需要 Unreal Engine 5.7+、同一项目中的 Unreal MCP 0.26.0+ 以及 C++ 工具链：发布包以源码形式提供，并为你的项目编译（在下次打开编辑器时，或使用 `--build` 立即编译）。安装 id 为 `com.ivanmurzak.unreal-ai-<id>`，其中 id 取自下表。安装选项（`--version`、`--build`、离线 `--source`）：[Installing an extension into a project](EXTENSIONS.md#installing-an-extension-into-a-project)。

| 扩展（id） | 说明 |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | 创建带骨骼、控制器和双骨骼 IK 解算器的绑定；查询 Control Rig Blueprint 和组件 |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | 查询 Input Action 和 Mapping Context，创建动作和上下文，映射按键 |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | 无需 C++ 即可编写 Gameplay Ability System：效果（消耗、冷却、伤害、恢复）、技能 Blueprint、带现成 Health / Mana / Stamina 属性集的技能系统组件、输入绑定、Play In Editor 检查 |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape 与 Water：创建、雕刻和绘制地形；创建海洋、湖泊和河流；查询地形、水体和水域区域 |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | Niagara 视觉特效：查询系统及其发射器，把系统放置到关卡中 |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | 程序化内容生成：编写 PCG 图表（节点、连接、设置、参数），把它们放到体积上，生成散布（森林、岩石、道具）并读取数量 |

## 按工具设置可见性

每个工具在 **MCP Tools** 窗口中都有一个 **Visible**（可见）复选框——即独立的 **MCP Tools** 标签页（注册在编辑器的 **Tools** 菜单下）。该窗口显示每个工具的标题、类别和说明，以及一行 "N / M tools visible" 摘要。**Visible** 只决定工具是否**列在 MCP `tools/list` 中**，即 AI 智能体是否能看到它。它不是访问控制：客户端按名称调用（`tools/call`）时，隐藏的工具仍会运行，而且 `unreal-tool-list` 也会列出隐藏的工具（标记为 hidden），以便智能体找到它们。

有两项设置起作用（见 [ARCHITECTURE](ARCHITECTURE.md) §7/§8）：

- **`enabledTools` 白名单**（可通过 `UNREAL_MCP_TOOLS` 覆盖）决定工具是否**被提供**——非空时只提供列出的工具，其他工具既不会列出也无法调用（`Execute()` 会拒绝）；为空表示“不过滤”；
- **`hiddenTools` 列表**——即你在 UI 中切换的 Visible 复选框——把已提供的工具从 `tools/list` 中隐藏。

两者都会跨编辑器会话保存，并且在扩展热重载后依然保留（重新注册的工具会继承保留的设置，因此重新构建后不会显示你隐藏的工具）。旧版本插件的配置文件仍然可用：其中的 `disabledTools` 列表会被当作 `hiddenTools` 读取，下次保存时写入 `hiddenTools`。

所有连接设置都位于唯一的 **AI Game Developer** 主窗口的 Connection 部分（没有单独的 Settings 标签页或 Project Settings 页面——与 Unity-MCP 保持一致）。**MCP Prompts** 和 **MCP Resources** 窗口已接入，但在此版本中为空——各自显示一条淡色的空状态提示（"N / M visible" 摘要仅 Tools 窗口才有）。
