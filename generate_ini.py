#!/usr/bin/env python3
"""Generate the INI and its two reference docs from the C++ source of truth.

Reads:
  - PatcherDLL/src/util/ini_registry.hpp   (simple toggle entries)
  - PatcherDLL/src/controller/controller_support.cpp  (per-mode button defaults,
    raw input names, action names)
  - version.h

Writes:
  - dist/BF2GameExt.ini
  - docs/user/CONFIGURATION.md
  - docs/user/CONTROLLER.md

The key/input/action *lists* come from the C++ tables; the prose describing each
one lives in this file. A name in the source with no prose here, or prose here
for a name no longer in the source, is a hard error rather than a silently
stale doc.

Run from the repo root:  python generate_ini.py
"""

import re
import sys
from collections import OrderedDict
from pathlib import Path

REPO = Path(__file__).resolve().parent
REGISTRY_HPP = REPO / "PatcherDLL" / "src" / "util" / "ini_registry.hpp"
CONTROLLER_CPP = REPO / "PatcherDLL" / "src" / "controller" / "controller_support.cpp"
VERSION_H = REPO / "version.h"
OUTPUT_INI = REPO / "dist" / "BF2GameExt.ini"
OUTPUT_CONFIG_MD = REPO / "docs" / "user" / "CONFIGURATION.md"
OUTPUT_CONTROLLER_MD = REPO / "docs" / "user" / "CONTROLLER.md"

DOC_HEADER = (
    "<!-- GENERATED FILE. Do not edit by hand.\n"
    "     Produced by generate_ini.py from ini_registry.hpp,\n"
    "     controller_support.cpp and version.h. Re-run:  python generate_ini.py -->"
)


# ── Prose tables ────────────────────────────────────────────────────────
# Everything below is hand-written documentation keyed by a name that must
# exist in the C++ source. Adding a feature means adding its line here too;
# the checks in verify_prose() will tell you if you forgot.

SECTION_BLURBS = OrderedDict([
    ("General",
     "Read by `dinput8.dll` before the extension is loaded, so these two cannot "
     "be changed at runtime."),
    ("LimitIncreases",
     "Engine limit patches. All are on by default and all are safe to leave on; "
     "they only raise a ceiling, they do not change behaviour below it."),
    ("Fixes",
     "Bug fixes for engine defects. On by default. Each one is guarded by a byte "
     "check against the stock instruction bytes, so a patch that does not "
     "recognise your executable declines to apply rather than corrupting it."),
    ("Features",
     "Optional behaviour that changes the game rather than fixing it. Some need "
     "assets that ship alongside the DLL."),
    ("Lightsaber",
     "Lighting for lightsaber blades. On by default. Radius and intensity are "
     "independent: radius changes how far the light reaches, intensity changes "
     "how bright it is, and changing one does not affect the other."),
    ("AI",
     "Removes hardcoded biases that make BF2's AI single out the human player. "
     "SWBF1 has no player term anywhere in its target selection, which is why "
     "its AI is remembered as fairer. Three of the four are on by default; set "
     "any of them to 0 for stock behaviour. `PlayerThreatFairness` is the "
     "exception and is **off** by default, because it is the one that stops the "
     "AI reacting to being aimed at, which makes them feel unresponsive rather "
     "than fair. AI will still turn on you the moment you damage them, because "
     "that path force-sets the attacker as the target and re-broadcasts to "
     "nearby squadmates; it is deliberate and is left alone."),
    ("Controller",
     "Gamepad support. The button and axis bindings live in the "
     "`[Controller.<Mode>]` sections and are documented separately in "
     "[CONTROLLER.md](CONTROLLER.md)."),
    ("AimAssist",
     "Xbox-style aim assist for gamepad players, singleplayer only. **Off by "
     "default** - set `Enabled=1` to turn it on, since it changes how aiming "
     "feels and mouse players have no use for it. The tuning values are set to "
     "match the console release, so enabling it alone gives you the Xbox feel; "
     "they are exposed for anyone who wants it stronger or weaker."),
])

MODE_DESCRIPTIONS = OrderedDict([
    ("Controller.Unit", "On foot, the default for infantry"),
    ("Controller.Vehicle", "Ground vehicles and walkers"),
    ("Controller.Flyer", "Flyers, adds `Roll`"),
    ("Controller.Hero", "Heroes and villains"),
    ("Controller.Turret", "Mounted and emplaced turrets"),
])

INPUT_DESCRIPTIONS = {
    "A": "Face button, bottom",
    "B": "Face button, right",
    "X": "Face button, left",
    "Y": "Face button, top",
    "LB": "Left shoulder bumper",
    "RB": "Right shoulder bumper",
    "Back": "Back / Select",
    "Start": "Start / Menu",
    "L3": "Left stick click",
    "R3": "Right stick click",
    "DPadUp": "D-pad up",
    "DPadRight": "D-pad right",
    "DPadDown": "D-pad down",
    "DPadLeft": "D-pad left",
    "LX+": "Left stick right",
    "LX-": "Left stick left",
    "LY+": "Left stick down",
    "LY-": "Left stick up",
    "ZPos": "DirectInput Z axis, positive. Usually the left trigger, see the note below",
    "ZNeg": "DirectInput Z axis, negative. Usually the right trigger, see the note below",
    "RX+": "Right stick right",
    "RX-": "Right stick left",
    "RY+": "Right stick down",
    "RY-": "Right stick up",
    "RZPos": "DirectInput RZ axis, positive. Varies by controller",
    "RZNeg": "DirectInput RZ axis, negative. Varies by controller",
    "RT": "Right trigger. Alias for `ZNeg`",
    "LT": "Left trigger. Alias for `ZPos`",
}

ACTION_DESCRIPTIONS = {
    "PrimaryFire": "Fire the primary weapon",
    "SecondaryFire": "Fire the secondary weapon",
    "Sprint": "Sprint",
    "Jump": "Jump",
    "Crouch": "Crouch. Double-tap triggers prone when the Prone feature is enabled",
    "Zoom": "Zoom / scope",
    "View": "Toggle first and third person",
    "Reload": "Reload",
    "Use": "Use / enter vehicle",
    "SquadCommand": "Issue a squad command",
    "AcceptHero": "Accept a hero spawn offer",
    "DeclineHero": "Decline a hero spawn offer",
    "LockTarget": "Lock onto a target",
    "PrimaryNext": "Next primary weapon",
    "PrimaryPrev": "Previous primary weapon",
    "SecondaryNext": "Next secondary weapon",
    "SecondaryPrev": "Previous secondary weapon",
    "PlayerList": "Show the player list",
    "Map": "Show the map",
    "Roll": "Roll (flyers)",
    "StrafeAxis": "Full strafe axis. Bind to a stick axis, not a button",
    "MoveAxis": "Full forward and back axis. Bind to a stick axis, not a button",
    "TurnAxis": "Full turn / yaw axis. Bind to a stick axis, not a button",
    "PitchAxis": "Full pitch axis. Bind to a stick axis, not a button",
    "StrafePos": "Strafe right. Half-axis, for binding a button to an axis",
    "StrafeNeg": "Strafe left. Half-axis, for binding a button to an axis",
    "MovePos": "Move backward. Half-axis, for binding a button to an axis",
    "MoveNeg": "Move forward. Half-axis, for binding a button to an axis",
    "TurnPos": "Turn right. Half-axis, for binding a button to an axis",
    "TurnNeg": "Turn left. Half-axis, for binding a button to an axis",
    "PitchPos": "Pitch down. Half-axis, for binding a button to an axis",
    "PitchNeg": "Pitch up. Half-axis, for binding a button to an axis",
    "None": "Explicitly unbound",
}


# ── Helpers ─────────────────────────────────────────────────────────────

def unescape(s: str) -> str:
    r"""Turn a C++ string literal's body into the text it actually denotes.

    The registry stores Windows paths, so `data\\_lvl_pc` in the source means
    `data\_lvl_pc`. Emitting the source form verbatim leaks doubled backslashes
    into the INI and the docs.
    """
    return s.replace('\\\\', '\\').replace('\\"', '"')


def md_cell(s: str) -> str:
    """Escape a string for use inside a markdown table cell."""
    return s.replace("|", "\\|")


def write_generated(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\r\n")
    print(f"Generated {path.relative_to(REPO)}")


# ── Parse version.h ─────────────────────────────────────────────────────

def parse_version(path: Path) -> str:
    """Return "MAJOR.MINOR.PATCH" from the repo-root version.h."""
    text = path.read_text(encoding="ascii")
    parts = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+GAMEEXT_VERSION_{field}\s+(\d+)", text)
        if not m:
            sys.exit(f"ERROR: GAMEEXT_VERSION_{field} not found in {path}")
        parts.append(m.group(1))
    return ".".join(parts)


# ── Parse ini_registry.hpp ──────────────────────────────────────────────

def parse_registry(path: Path):
    """Yield (section, key, default, comment) from INI_ENTRY / INI_PATCH lines."""
    text = path.read_text(encoding="utf-8")

    # Extract the block between BEGIN_REGISTRY and END_REGISTRY
    m = re.search(r"// BEGIN_REGISTRY\s*\n(.*?)// END_REGISTRY", text, re.DOTALL)
    if not m:
        sys.exit("ERROR: Could not find BEGIN_REGISTRY / END_REGISTRY in " + str(path))
    block = m.group(1)

    # Match INI_ENTRY("sec", "key", "def", "comment")
    # and   INI_PATCH("sec", "key", "def", "comment", "patch_name")
    pattern = re.compile(
        r'INI_(?:ENTRY|PATCH)\(\s*"([^"]*?)"\s*,\s*"([^"]*?)"\s*,\s*"([^"]*?)"\s*,\s*"((?:[^"\\]|\\.)*)"'
    )
    for m in pattern.finditer(block):
        yield m.group(1), m.group(2), m.group(3), unescape(m.group(4))


# ── Parse controller_support.cpp ────────────────────────────────────────

def parse_controller_modes(text: str):
    """Return OrderedDict  section_name -> [(input, actions), ...]"""
    # Find s_modeSectionNames
    m = re.search(
        r's_modeSectionNames\s*\[.*?\]\s*=\s*\{(.*?)\};', text, re.DOTALL
    )
    if not m:
        sys.exit("ERROR: Could not find s_modeSectionNames in " + str(CONTROLLER_CPP))
    section_names = re.findall(r'"([^"]+)"', m.group(1))

    # Find each *Defaults[] array.  They appear in order matching s_modeDefaults.
    # s_modeDefaults lists them as: s_unitDefaults, s_vehicleDefaults, ...
    m = re.search(r's_modeDefaults\s*\[.*?\]\s*=\s*\{(.*?)\};', text, re.DOTALL)
    if not m:
        sys.exit("ERROR: Could not find s_modeDefaults in " + str(CONTROLLER_CPP))
    array_names = re.findall(r'(s_\w+Defaults)', m.group(1))

    if len(array_names) != len(section_names):
        sys.exit(
            f"ERROR: {len(array_names)} default arrays but {len(section_names)} section names"
        )

    modes = OrderedDict()
    for arr_name, sec_name in zip(array_names, section_names):
        pat = re.compile(
            rf'{re.escape(arr_name)}\s*\[\]\s*=\s*\{{(.*?)\{{\s*nullptr', re.DOTALL
        )
        am = pat.search(text)
        if not am:
            sys.exit(f"ERROR: Could not find array body for {arr_name}")
        body = am.group(1)
        bindings = re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\}', body)
        modes[sec_name] = bindings

    return modes


def parse_name_table(text: str, array_name: str):
    """Return the names from a NamedValue table, in source order.

    Matches  { "Name", SOME_ENUM },  and stops at the { nullptr, 0 } sentinel.
    """
    pat = re.compile(
        rf'{re.escape(array_name)}\s*\[\]\s*=\s*\{{(.*?)\{{\s*nullptr', re.DOTALL
    )
    m = pat.search(text)
    if not m:
        sys.exit(f"ERROR: Could not find array body for {array_name}")
    return re.findall(r'\{\s*"([^"]+)"\s*,\s*\w+\s*\}', m.group(1))


# ── Prose / source agreement checks ─────────────────────────────────────

def verify_prose(entries, modes, inputs, actions):
    """Fail loudly when the C++ tables and the prose here have drifted apart.

    A missing description means someone added a feature and did not document it.
    A leftover description means the doc still advertises something that is gone.
    Either way the generated docs would be wrong, so refuse to write them.
    """
    problems = []

    def compare(kind, source_names, prose_names):
        for n in source_names:
            if n not in prose_names:
                problems.append(f"{kind} '{n}' is in the source but has no description "
                                f"in generate_ini.py")
        for n in prose_names:
            if n not in source_names:
                problems.append(f"{kind} '{n}' is described in generate_ini.py but no "
                                f"longer exists in the source")

    compare("INI section", [s for s, _, _, _ in entries], list(SECTION_BLURBS))
    compare("Controller mode", list(modes), list(MODE_DESCRIPTIONS))
    compare("Raw input", inputs, INPUT_DESCRIPTIONS)
    compare("Action", actions, ACTION_DESCRIPTIONS)

    for section, key, _, comment in entries:
        if not comment:
            problems.append(f"[{section}] {key} has no comment in ini_registry.hpp")

    if problems:
        for p in problems:
            print(f"ERROR: {p}")
        sys.exit("Refusing to generate. Fix the mismatches above.")


# ── Write INI ───────────────────────────────────────────────────────────

def group_sections(entries):
    """Group registry entries by section, preserving source order."""
    sections = OrderedDict()
    for section, key, default, comment in entries:
        sections.setdefault(section, []).append((key, default, comment))
    return sections


def write_ini(entries, modes, version, path: Path):
    lines = []
    lines.append(f"; BF2GameExt.ini - auto-generated by generate_ini.py for v{version}")
    lines.append("; To regenerate, run python generate_ini.py")
    lines.append("")

    # Write simple sections from registry
    for section, keys in group_sections(entries).items():
        lines.append(f"[{section}]")
        for key, default, comment in keys:
            if comment:
                lines.append(f"; {comment}")
            lines.append(f"{key}={default}")
        lines.append("")

    # Write controller binding sections
    lines.append("; Controller button/axis bindings per mode.")
    lines.append("; Keys are raw input names, values are comma-separated action names.")
    lines.append("; Omit a key or set it to empty to unbind.  Defaults are shown below.")
    lines.append("; Full list of input and action names: docs/user/CONTROLLER.md")
    lines.append("")

    for sec_name, bindings in modes.items():
        lines.append(f"[{sec_name}]")
        for input_name, actions in bindings:
            if actions:
                lines.append(f";{input_name}={actions}")
            # Skip empty defaults (unbound by default)
        lines.append("")

    write_generated(path, "\n".join(lines) + "\n")


# ── Write CONFIGURATION.md ──────────────────────────────────────────────

def write_configuration_md(entries, version, path: Path):
    L = [DOC_HEADER, "", "# Configuration", ""]
    L.append(
        f"Every option BF2GameExt v{version} reads from `BF2GameExt.ini`, which sits "
        f"next to the DLL in `GameData`. The INI is only consulted by the DInput8 "
        f"proxy install, and it is optional: with no file present every key falls "
        f"back to the default listed below."
    )
    L.append("")
    L.append(
        "Values are `1` for on and `0` for off unless a range is given. Lines "
        "starting with `;` are comments."
    )
    L.append("")

    for section, keys in group_sections(entries).items():
        L.append(f"## {section}")
        L.append("")
        L.append(SECTION_BLURBS[section])
        L.append("")
        L.append("| Key | Default | Description |")
        L.append("|-----|---------|-------------|")
        for key, default, comment in keys:
            L.append(f"| `{key}` | `{default}` | {md_cell(comment)} |")
        L.append("")

    L.append("## Controller bindings")
    L.append("")
    L.append(
        "The `[Controller.Unit]`, `[Controller.Vehicle]`, `[Controller.Flyer]`, "
        "`[Controller.Hero]` and `[Controller.Turret]` sections map physical "
        "buttons and axes to in-game actions. Every default is written into the "
        "shipped INI as a commented-out line. See "
        "[CONTROLLER.md](CONTROLLER.md) for the input and action names and the "
        "full default tables."
    )
    L.append("")
    L.append(
        "Not everything is configured here. Gameplay features also read `load.cfg` "
        "parameters, ODF properties and Lua functions; see "
        "[FEATURES.md](FEATURES.md)."
    )

    write_generated(path, "\n".join(L) + "\n")


# ── Write CONTROLLER.md ─────────────────────────────────────────────────

def write_controller_md(modes, inputs, actions, version, path: Path):
    L = [DOC_HEADER, "", "# Controller Bindings", ""]
    L.append(
        f"Gamepad binding reference for BF2GameExt v{version}. Enable the pad "
        f"itself with `[Controller] Enabled=1`. Aim assist is separate and is "
        f"**off by default** - turn it on with `[AimAssist] Enabled=1`. See "
        f"[CONFIGURATION.md](CONFIGURATION.md#controller) for both and for the "
        f"aim assist tuning values."
    )
    L.append("")

    L.append("## How a binding works")
    L.append("")
    L.append(
        "Each binding is one line in a `[Controller.<Mode>]` section. The **key** "
        "is a raw input (a physical button or axis) and the **value** is a "
        "comma-separated list of **actions** to fire:"
    )
    L.append("")
    L.append("```ini")
    L.append("[Controller.Unit]")
    L.append("A=Jump              ; one button, one action")
    L.append("LB=Crouch,Zoom      ; one button, two actions at once")
    L.append("LY-=MoveAxis        ; a stick axis driving a full movement axis")
    L.append("DPadUp=MoveNeg      ; a button driving one half of an axis")
    L.append("Back=               ; empty value unbinds it")
    L.append("```")
    L.append("")
    L.append(
        "In the shipped INI every default line is commented out with a leading "
        "`;`. Uncomment a line to override that binding; anything you leave "
        "commented keeps its default. Bindings are per mode and do not inherit, so "
        "rebinding jump for `Controller.Unit` does not change it for "
        "`Controller.Hero`."
    )
    L.append("")
    L.append(
        "Full axes (`MoveAxis`, `TurnAxis`, `StrafeAxis`, `PitchAxis`) expect a "
        "stick axis on the left side. The half-axis actions (`MovePos`/`MoveNeg` "
        "and friends) exist so you can drive movement from a digital button such "
        "as a d-pad direction."
    )
    L.append("")

    L.append("## Modes")
    L.append("")
    L.append("| Section | Applies to |")
    L.append("|---------|------------|")
    for sec_name in modes:
        L.append(f"| `[{sec_name}]` | {MODE_DESCRIPTIONS[sec_name]} |")
    L.append("")

    L.append("## Raw input names")
    L.append("")
    L.append("Valid on the left of the `=`.")
    L.append("")
    L.append("| Name | Control |")
    L.append("|------|---------|")
    for name in inputs:
        L.append(f"| `{name}` | {md_cell(INPUT_DESCRIPTIONS[name])} |")
    L.append("")
    L.append(
        "> **Triggers.** Both triggers sit on the single DirectInput Z axis on "
        "most pads, with the left trigger reading positive and the right negative. "
        "`LT` and `RT` are aliases for `ZPos` and `ZNeg`, so binding both a trigger "
        "alias and its Z axis name to different actions will not do what you want. "
        "Which physical control lands on `RZPos`/`RZNeg` varies between controllers "
        "and drivers, so those two are worth testing rather than assuming."
    )
    L.append("")

    L.append("## Action names")
    L.append("")
    L.append("Valid on the right of the `=`, comma-separated.")
    L.append("")
    L.append("| Name | Effect |")
    L.append("|------|--------|")
    for name in actions:
        L.append(f"| `{name}` | {md_cell(ACTION_DESCRIPTIONS[name])} |")
    L.append("")

    L.append("## Default bindings")
    L.append("")
    L.append("Blank means unbound by default.")
    L.append("")
    for sec_name, bindings in modes.items():
        L.append(f"### {sec_name}")
        L.append("")
        L.append("| Input | Actions |")
        L.append("|-------|---------|")
        for input_name, acts in bindings:
            cell = f"`{acts}`" if acts else "-"
            L.append(f"| `{input_name}` | {cell} |")
        L.append("")

    write_generated(path, "\n".join(L[:-1]) + "\n")


# ── Main ────────────────────────────────────────────────────────────────

def main():
    if not REGISTRY_HPP.exists():
        sys.exit(f"ERROR: {REGISTRY_HPP} not found (run from repo root)")
    if not CONTROLLER_CPP.exists():
        sys.exit(f"ERROR: {CONTROLLER_CPP} not found")

    version = parse_version(VERSION_H)
    entries = list(parse_registry(REGISTRY_HPP))

    controller_text = CONTROLLER_CPP.read_text(encoding="utf-8")
    modes = parse_controller_modes(controller_text)
    inputs = parse_name_table(controller_text, "s_rawInputNames")
    actions = parse_name_table(controller_text, "s_actionNames")

    verify_prose(entries, modes, inputs, actions)

    write_ini(entries, modes, version, OUTPUT_INI)
    write_configuration_md(entries, version, OUTPUT_CONFIG_MD)
    write_controller_md(modes, inputs, actions, version, OUTPUT_CONTROLLER_MD)

    # Summary
    n_keys = len(entries) + sum(len(b) for b in modes.values())
    print(f"  v{version}, {len(entries)} registry entries, {len(modes)} controller modes, "
          f"{n_keys} total keys")
    print(f"  {len(inputs)} raw inputs, {len(actions)} actions")


if __name__ == "__main__":
    main()
