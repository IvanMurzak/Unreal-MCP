"""Regenerate the README images (hero, architecture, terminal) with the shared svgkit style.

Usage: py -3 docs/img/readme/build-svgs.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svgkit as K  # noqa: E402


def out(name):
    return os.path.join(HERE, name)


# ── hero ────────────────────────────────────────────────────────────────────────────────────────────
K.write(out("hero.svg"), K.hero(
    title="Unreal ", accent="MCP", eyebrow_text="AI Game Developer",
    subtitle=["Let any AI agent inspect and drive", "the Unreal Editor."],
    chip_items=["UE 5.5–5.8", "npm i -g unreal-mcp-cli"],
    label="Unreal MCP — let any AI agent inspect and drive the Unreal Editor"))

# ── architecture ────────────────────────────────────────────────────────────────────────────────────
W, H = 1200, 480
CW, GAP, X0, Y = 220, 80, 40, 104
xs = [X0 + i * (CW + GAP) for i in range(4)]
body = [K.eyebrow(X0, 72, "How it works")]
cards = [
    ("Your AI agent", "Claude Code, Cursor,", "Copilot, Codex, …", "blue"),
    ("MCP server", "ai-game.dev cloud or", "gamedev-mcp-server", "blue"),
    ("Sidecar", "unreal-mcp-bridge", ".NET 9, auto-started", "yellow"),
    ("UnrealMCP plugin", "C++, inside the editor", "tools on game thread", "red"),
]
for x, (title, sub, sub2, accent) in zip(xs, cards):
    body.append(K.card(x, Y, CW, 130, title, sub=sub, accent=accent, sub2=sub2))
for i, label in enumerate(["MCP", "SignalR", "TCP"]):
    gx = xs[i] + CW
    mid = gx + GAP / 2
    body.append(f'<text x="{mid}" y="{Y + 44}" text-anchor="middle" class="s" font-size="16" font-weight="700" fill="{K.SECOND}">{label}</text>')
    body.append(K.wire(f"M{gx + 6} {Y + 62} L{gx + GAP - 6} {Y + 62}", color=K.BLUE, dur=1.6, begin=i * 0.5))
    body.append(K.wire(f"M{gx + 6} {Y + 82} L{gx + GAP - 6} {Y + 82}", color=K.RED_TEXT, dur=1.6, begin=0.8 + i * 0.5, reverse=True))

body.append(K.eyebrow(X0, 296, "30 built-in tools · 8 families"))
fams = ["RQL data · 3", "Blueprint · 11", "C++ source · 6", "Screenshot · 4",
        "Fab · 3", "Console · 1", "Python · 1", "Tool list · 1"]
PW = 268
for i, f in enumerate(fams):
    s, _ = K.pill(X0 + (i % 4) * (PW + 16), 316 + (i // 4) * 48, f, w=PW)
    body.append(s)
body.append(f'<text x="{W / 2}" y="446" text-anchor="middle" class="s" font-size="16" fill="{K.MUTED}">'
            'Loopback only: the plugin listens, the sidecar dials in with a one-shot token.</text>')
K.write(out("architecture.svg"), K.panel(W, H, "".join(body),
        label="An AI agent talks MCP to the server (ai-game.dev cloud or a local gamedev-mcp-server); the server relays "
              "over SignalR to the unreal-mcp-bridge .NET sidecar, which reaches the UnrealMCP C++ plugin in the "
              "Unreal Editor over loopback TCP. 30 built-in tools in 8 families."))

# ── terminal (message formats are the CLI's own) ───────────────────────────────────────────────────
K.write(out("terminal.svg"), K.terminal(
    title="~/games — unreal-mcp-cli quick start",
    label="A terminal session: install unreal-mcp-cli, install the UnrealMCP plugin, sign in, configure Claude Code, open the Unreal Editor",
    open_finished=True,
    lines=[
        {"cmd": "npm install -g unreal-mcp-cli"},
        {"cmd": "unreal-mcp-cli install-plugin ./MyGame"},
        {"out": [("ok", "  ✔  "), ("text", "Installed plugin (copy) at ~/games/MyGame/Plugins/UnrealMCP")]},
        {"cmd": "unreal-mcp-cli login"},
        {"out": [("ok", "  ✔  "), ("text", "Logged in.")], "note": "once per machine"},
        {"cmd": "unreal-mcp-cli setup-mcp claude-code -p ./MyGame"},
        {"out": [("ok", "  ✔  "), ("text", "Wrote claude-code MCP config (http) to "), ("link", "MyGame/.mcp.json")]},
        {"cmd": "unreal-mcp-cli open ./MyGame"},
        {"out": [("ok", "  ✔  "), ("text", "Launched Unreal Editor (PID: 18424)")], "note": "plugin starts the bridge"},
        {"out": [("muted", "  # ask your agent: "), ("str", "\"Spawn three cubes and a point light above them\"")], "pause": 0.8},
    ]))
