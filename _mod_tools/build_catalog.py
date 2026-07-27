"""Generate the in-game mod catalog screen from mod manifests.

The catalog is rebuilt from scratch every time: this reads the pristine
Hangar.yaml out of backup/, splices a freshly generated ModCatalogScreen into
it, and installs the result. Nothing is patched on top of a previous patch, so
a broken run can never accumulate.

    python build_catalog.py            # rebuild from the registry and install
    python build_catalog.py --local    # use _mod_tools/mods/*/manifest.yaml instead
    python build_catalog.py --dry-run  # print the generated screen only

Mods live in _mod_tools/mods/<id>/manifest.yaml:

    id: night-mode
    name: Ночной режим
    version: 1.2.0
    author: pseud
    description: Ночная цветокоррекция сцены боя
    long: |
      Multi-line text for the detail page.
    downloads: 1240
    updated: 27.07.2026
    type: resource
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODS = HERE / "mods"
WORK = HERE / "work"
BACKUP = HERE / "backup"
HANGAR_REL = "UI/Screens3/Lobby/Hangar/Hangar.yaml"
PYTHON = sys.executable

# cards sit two levels deeper now that the list scrolls:
# ListPage > UIScrollView > UIScrollViewContainer > children
CARD_INDENT = 20

ICON = "~res:/Gfx/Lobby/icons/icon_settings_n"
STYLES = ("~res:/UI/Screens3/Color.style.yaml;"
          "~res:/UI/Screens3/Font.style.yaml;"
          "~res:/UI/Screens3/Lobby/Hangar/DevMenu/SimpleButton.style.yaml")


# --------------------------------------------------------------- manifests

def read_manifest(path: Path) -> dict:
    """Deliberately tiny parser: key: value plus one `long: |` block.

    Pulling in PyYAML would mean every user of the toolchain needs it, and the
    manifest format is fixed and flat by design.
    """
    data, key, block = {}, None, []
    for raw in path.read_text(encoding="utf-8").splitlines():
        if key:                                   # inside a `|` block
            if raw.startswith(("  ", "\t")) or not raw.strip():
                block.append(raw.strip())
                continue
            data[key] = "\n".join(block).strip()
            key, block = None, []
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        name, _, value = raw.partition(":")
        name, value = name.strip(), value.strip()
        if value == "|":
            key = name
        else:
            data[name] = value
    if key:
        data[key] = "\n".join(block).strip()
    return data


def load_mods(source: str = "registry") -> list[dict]:
    """Mod metadata for the catalogue screen.

    The registry is the real source; local manifests stay available for
    authoring a mod before it has been published. registry.fetch already falls
    back to its disk cache, so a server outage degrades to stale data rather
    than an empty hangar.
    """
    if source == "registry":
        import registry as registry_client
        import install as installer

        entries, origin = registry_client.fetch()
        print(f"registry: {origin}")
        ledger = installer.load_ledger()
        mods = []
        for entry in entries:
            mod = {key: str(entry.get(key, "")) for key in
                   ("id", "name", "version", "author", "description",
                    "long", "type", "downloads", "updated")}
            mod["long"] = mod["long"] or mod["description"]
            mod["installed"] = "true" if entry["id"] in ledger else "false"
            mods.append(mod)
        return mods

    return load_local_mods()


def load_local_mods() -> list[dict]:
    if not MODS.exists():
        return []
    mods = []
    for manifest in sorted(MODS.glob("*/manifest.yaml")):
        m = read_manifest(manifest)
        m.setdefault("id", manifest.parent.name)
        m.setdefault("name", m["id"])
        m.setdefault("author", "unknown")
        m.setdefault("version", "1.0.0")
        m.setdefault("description", "")
        m.setdefault("long", m["description"])
        m.setdefault("downloads", "0")
        m.setdefault("updated", "")
        m.setdefault("type", "resource")
        m.setdefault("installed", "false")
        mods.append(m)
    return mods


# ------------------------------------------------------------- yaml pieces

def esc(text: str) -> str:
    """Quote a literal for a DAVA binding expression."""
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def text_control(name, cls, x, y, w, h, literal, indent):
    p = " " * indent
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "{name}"\n'
        f'{p}    size: [{w:.6f}, {h:.6f}]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "{cls}"\n'
        f'{p}    components:\n'
        f'{p}        UITextComponent:\n'
        f'{p}            colorInheritType: "COLOR_IGNORE_PARENT"\n'
        f'{p}            multiline: "MULTILINE_DISABLED"\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            leftAnchor: {x:.6f}\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: {y:.6f}\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {w:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {h:.6f}\n'
        f'{p}    bindings:\n'
        f'{p}    - ["UITextComponent.text", "\\"{esc(literal)}\\""]\n'
    )


CARD_BUTTON_ANCHOR = ("rightAnchorEnabled: true",
                      "rightAnchor: 20.000000",
                      "vCenterAnchorEnabled: true")
DETAIL_BUTTON_ANCHOR = ("leftAnchorEnabled: true",
                        "leftAnchor: 168.000000",
                        "topAnchorEnabled: true",
                        "topAnchor: 80.000000")


def install_button(indent, index=0, installed=False, sent_expr=None,
                   width=168.0, height=48.0, anchor=CARD_BUTTON_ANCHOR):
    """The anchor block is a parameter, never post-hoc string surgery.

    Rewriting the emitted YAML with str.replace() hits every occurrence, not
    the one intended: it duplicated a key inside the Caption's Anchor and
    pasted a line at the caller's indentation instead of the block's, which
    is a parse error rather than a layout glitch.
    """
    p = " " * indent
    anchor_block = "".join(f'{p}            {line}\n' for line in anchor)
    # One button, two jobs. An installed mod's only useful action is removal,
    # so the button becomes it rather than sitting there greyed out next to a
    # second control the user has to hunt for.
    action = "ON_MOD_REMOVE_CLICKED" if installed else "ON_MOD_INSTALL_CLICKED"
    # Only the card that was pressed reacts. modRequestSent alone is one flag
    # for the whole screen, so keying on it without the index latched every
    # button at once and the list stopped showing install state entirely.
    #
    # The condition cannot be written into the captions directly. This button
    # carries a UIDataParamsComponent, which opens its own data scope, and the
    # screen's local variables are not visible inside it - which is why both a
    # `when` on the text and a `visible` binding silently did nothing while a
    # plain literal drew fine. A declared param with an arg is how the game
    # itself passes a value across that boundary.
    shown = sent_expr or state_expression(index, installed)
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "InstallButton"\n'
        f'{p}    size: [{width:.6f}, {height:.6f}]\n'
        f'{p}    classes: "simple-button"\n'
        f'{p}    components:\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        UIInputEventComponent:\n'
        f'{p}            onTouchUpInside: "ON_CLICK"\n'
        f'{p}        UIDataParamsComponent:\n'
        f'{p}            params:\n'
        f'{p}            - ["bool", "shown", "false", "false"]\n'
        f'{p}            args:\n'
        f'{p}                "shown": "{shown}"\n'
        f'{p}            events:\n'
        f'{p}            - "ON_CLICK"\n'
        f'{p}            eventActions:\n'
        f'{p}            - ["ON_CLICK", "{action}", "{index}"]\n'
        f'{p}        Anchor:\n'
        f'{anchor_block}'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {width:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {height:.6f}\n'
        f'{p}    children:\n'
        f'{caption_control(p, "FaceInstall", "green-la-palma-bg", "УСТАНОВИТЬ", "not shown")}'
        f'{caption_control(p, "FaceRemove", "red-tamarillo-bg", "УДАЛИТЬ", "shown")}'
    )


def state_expression(index: int, installed: bool) -> str:
    """Whether the mod reads as installed right now, pending action included.

    Until the client restarts the ledger cannot change, so the button has to
    show the state the press will produce rather than the one on disk. `shown`
    is the state after the pending request, falling back to the built-in one
    when this card has no request outstanding.
    """
    touched = f"modRequestSent and modRequestIndex == {index}"
    if installed:
        return f"not ({touched}) or modPendingInstalled"
    return f"{touched} and modPendingInstalled"


def caption_control(p: str, name: str, tint: str, text: str, visible: str) -> str:
    """One of the button's two faces: its own colour and its own label.

    The colour has to change with the state as well as the text, and a control
    only carries one set of classes, so each state is a full-size face of its
    own and `visible` picks between them.

    The label used to switch through a `when` expression on
    UITextComponent.text and rendered as nothing in game - a green button with
    no writing on it. The two states are now two controls toggled by `visible`,
    which this screen already relies on for its pages and its overlay, so it is
    a mechanism known to work here rather than one assumed to.
    """
    return (
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "{name}"\n'
        f'{p}        input: false\n'
        f'{p}        classes: "t-button bold white-wild-sand-text {tint}"\n'
        f'{p}        components:\n'
        f'{p}            Background: {{}}\n'
        f'{p}            UITextComponent:\n'
        f'{p}                colorInheritType: "COLOR_IGNORE_PARENT"\n'
        f'{p}                multiline: "MULTILINE_DISABLED"\n'
        f'{p}                align: ["HCENTER", "VCENTER"]\n'
        f'{p}            Anchor:\n'
        f'{p}                leftAnchorEnabled: true\n'
        f'{p}                rightAnchorEnabled: true\n'
        f'{p}                topAnchorEnabled: true\n'
        f'{p}                bottomAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "PercentOfParent"\n'
        f'{p}                verticalPolicy: "PercentOfParent"\n'
        f'{p}        bindings:\n'
        f'{p}        - ["visible", "{visible}"]\n'
        f'{p}        - ["UITextComponent.text", "\\"{text}\\""]\n'
    )


def card(mod: dict, index: int, indent: int = CARD_INDENT) -> str:
    p = " " * indent
    meta = f'{mod["author"]}  •  {mod["downloads"]} загрузок  •  {mod["updated"]}'
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "Card{index}"\n'
        f'{p}    size: [944.000000, 120.000000]\n'
        f'{p}    classes: "simple-button grey-shark-70-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        UIInputEventComponent:\n'
        f'{p}            onTouchUpInside: "ON_CLICK"\n'
        f'{p}        UIDataParamsComponent:\n'
        # The card opens a data scope of its own, so the button inside it
        # cannot reach modRequestSent either - and an arg on the button is
        # evaluated here, in this scope, not at the screen. The value has to be
        # carried across both boundaries, one hop at a time.
        f'{p}            params:\n'
        f'{p}            - ["bool", "shown", "false", "false"]\n'
        f'{p}            args:\n'
        f'{p}                "shown": "{state_expression(index, mod["installed"] == "true")}"\n'
        f'{p}            events:\n'
        f'{p}            - "ON_CLICK"\n'
        f'{p}            eventActions:\n'
        f'{p}            - ["ON_CLICK", "ON_MOD_CARD_CLICKED", "{index}"]\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: 120.000000\n'
        f'{p}    children:\n'
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "Icon"\n'
        f'{p}        size: [88.000000, 88.000000]\n'
        f'{p}        input: false\n'
        f'{p}        classes: "black-25-bg"\n'
        f'{p}        components:\n'
        f'{p}            Background:\n'
        f'{p}                drawType: "DRAW_ALIGNED"\n'
        f'{p}                sprite: "{ICON}"\n'
        f'{p}                align: ["HCENTER", "VCENTER"]\n'
        f'{p}            Anchor:\n'
        f'{p}                leftAnchorEnabled: true\n'
        f'{p}                leftAnchor: 16.000000\n'
        f'{p}                vCenterAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "FixedSize"\n'
        f'{p}                horizontalValue: 88.000000\n'
        f'{p}                verticalPolicy: "FixedSize"\n'
        f'{p}                verticalValue: 88.000000\n'
    )
    out += text_control("Name", "t-subtitle bold align-left white-wild-sand-text",
                        120, 18, 520, 30, mod["name"], indent + 4)
    out += text_control("Meta", "t-caption regular align-left white-wild-sand-50-text",
                        120, 50, 520, 22, meta, indent + 4)
    out += text_control("Description", "t-body regular align-left white-wild-sand-70-text",
                        120, 76, 560, 24, mod["description"], indent + 4)
    out += install_button(indent + 4, index=index,
                          installed=mod["installed"] == "true",
                          sent_expr="shown")
    return out


def build_screen(mods: list[dict]) -> str:
    cards = "".join(card(m, i, CARD_INDENT) for i, m in enumerate(mods))
    if not cards:
        cards = text_control("Empty", "t-body regular align-left white-wild-sand-50-text",
                             0, 0, 600, 30, "Установленных модов нет", CARD_INDENT)
    # The header shows the open mod's name on the detail page and the catalog
    # title with a count on the list, so one chained `when` covers both.
    if mods:
        cases = ", ".join(f'modDetailIndex == {i} -> \\"{esc(m["name"]).upper()}\\"'
                          for i, m in enumerate(mods))
        detail_title = f'(when {cases}, \\"МОД\\")'
    else:
        detail_title = '\\"МОД\\"'
    title_expr = (f'when modDetailVisible -> {detail_title}, '
                  f'\\"КАТАЛОГ МОДОВ · {len(mods)}\\"')

    head = f'''    -   class: "UIControl"
        name: "ModCatalogScreen"
        size: [1024.000000, 768.000000]
        input: true
        components:
            Background:
                drawType: "DRAW_FILL"
                color: [0.043137, 0.058824, 0.078431, 0.720000]
            IgnoreLayout: {{}}
            Anchor:
                leftAnchorEnabled: true
                rightAnchorEnabled: true
                topAnchorEnabled: true
                bottomAnchorEnabled: true
            SizePolicy:
                horizontalPolicy: "PercentOfParent"
                verticalPolicy: "PercentOfParent"
            StyleSheet:
                styles: "{STYLES}"
        bindings:
        - ["visible", "modCatalogVisible"]
        children:
        -   class: "UIControl"
            name: "RequestSignal"
            size: [1.000000, 1.000000]
            input: false
            components:
                Background: {{}}
                IgnoreLayout: {{}}
                Anchor:
                    leftAnchorEnabled: true
                    topAnchorEnabled: true
                SizePolicy:
                    horizontalPolicy: "FixedSize"
                    horizontalValue: 1.000000
                    verticalPolicy: "FixedSize"
                    verticalValue: 1.000000
            bindings:
            - ["Background.sprite", "when modRequestSent -> \\"~res:/BLITZFORGE/\\" + str(modRequestVerb) + \\"/\\" + str(modRequestIndex) + \\"-\\" + str(modRequestSeq), \\"\\""]
        -   class: "UIControl"
            name: "HeaderBar"
            size: [1024.000000, 80.000000]
            input: false
            classes: "black-50-bg"
            components:
                Background: {{}}
                Anchor:
                    leftAnchorEnabled: true
                    rightAnchorEnabled: true
                    topAnchorEnabled: true
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "FixedSize"
                    verticalValue: 80.000000
            children:
            -   class: "UIControl"
                name: "BackSquare"
                size: [72.000000, 72.000000]
                input: false
                classes: "grey-shark-70-bg"
                components:
                    Background: {{}}
                    Anchor:
                        leftAnchorEnabled: true
                        leftAnchor: 4.000000
                        vCenterAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 72.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 72.000000
                children:
                -   prototype: "IconButtonWithBadge/IconButton"
                    name: "BackButton"
                    components:
                        Anchor:
                            hCenterAnchorEnabled: true
                            vCenterAnchorEnabled: true
                        UIDataParamsComponent:
                            args:
                                "image": "\\"~res:/Gfx/Lobby/icons/icon_arrow-back\\""
                                "type": "eButtonType.NO_BG"
                            eventActions:
                            - ["ON_CLICK_BUTTON", "ON_MOD_CATALOG_BACK", ""]
            -   class: "UIControl"
                name: "ScreenTitle"
                size: [620.000000, 40.000000]
                input: false
                classes: "t-title bold align-left white-wild-sand-text"
                components:
                    UITextComponent:
                        colorInheritType: "COLOR_IGNORE_PARENT"
                        multiline: "MULTILINE_DISABLED"
                    Anchor:
                        leftAnchorEnabled: true
                        leftAnchor: 96.000000
                        vCenterAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 620.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 40.000000
                bindings:
                - ["UITextComponent.text", "{title_expr}"]
        -   class: "UIControl"
            name: "ListPage"
            size: [944.000000, 616.000000]
            input: false
            components:
                Anchor:
                    leftAnchorEnabled: true
                    leftAnchor: 40.000000
                    rightAnchorEnabled: true
                    rightAnchor: 40.000000
                    topAnchorEnabled: true
                    topAnchor: 104.000000
                    bottomAnchorEnabled: true
                    bottomAnchor: 32.000000
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "PercentOfParent"
            bindings:
            - ["visible", "not modDetailVisible"]
            - ["Anchor.topAnchor", "when modRequestSent -> 176, 104"]
            children:
            -   class: "UIScrollView"
                name: "ModScroll"
                size: [944.000000, 616.000000]
                autoUpdate: true
                centerContent: false
                components:
                    SizePolicy:
                        horizontalPolicy: "PercentOfParent"
                        verticalPolicy: "PercentOfParent"
                children:
                -   class: "UIScrollViewContainer"
                    name: "scrollContainerControl"
                    components:
                        LinearLayout:
                            orientation: "TopDown"
                            spacing: 12.000000
                        SizePolicy:
                            horizontalPolicy: "PercentOfParent"
                            verticalPolicy: "PercentOfChildrenSum"
                    children:
{cards}'''

    pages = "".join(detail_page(m, i) for i, m in enumerate(mods))
    return head + pages + restart_bar() + confirm_overlay()


def detail_page(mod: dict, index: int) -> str:
    """One page per mod, selected by modDetailIndex.

    A single shared page could only ever show mods[0], so every card opened
    the same mod regardless of which was tapped.
    """
    detail_meta = f'Версия {mod["version"]}  •  автор {mod["author"]}'
    detail_stats = (f'{mod["downloads"]} загрузок  •  обновлён {mod["updated"]}'
                    f'  •  {mod["type"]}-мод')

    detail = f'''        -   class: "UIControl"
            name: "DetailPage{index}"
            size: [944.000000, 616.000000]
            input: false
            components:
                Anchor:
                    leftAnchorEnabled: true
                    leftAnchor: 40.000000
                    rightAnchorEnabled: true
                    rightAnchor: 40.000000
                    topAnchorEnabled: true
                    topAnchor: 104.000000
                    bottomAnchorEnabled: true
                    bottomAnchor: 32.000000
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "PercentOfParent"
            bindings:
            - ["visible", "modDetailVisible and modDetailIndex == {index}"]
            - ["Anchor.topAnchor", "when modRequestSent -> 176, 104"]
            children:
            -   class: "UIControl"
                name: "DetailIcon"
                size: [140.000000, 140.000000]
                input: false
                classes: "black-25-bg"
                components:
                    Background:
                        drawType: "DRAW_ALIGNED"
                        sprite: "{ICON}"
                        align: ["HCENTER", "VCENTER"]
                    Anchor:
                        leftAnchorEnabled: true
                        topAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 140.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 140.000000
'''
    detail += text_control("DetailMeta", "t-caption regular align-left white-wild-sand-50-text",
                           168, 8, 600, 26, detail_meta, 12)
    detail += text_control("DetailStats", "t-caption regular align-left white-wild-sand-50-text",
                           168, 40, 600, 26, detail_stats, 12)
    detail += install_button(12, index=index,
                             installed=mod["installed"] == "true",

                             width=200.0, height=52.0,
                             anchor=DETAIL_BUTTON_ANCHOR)
    long_text = mod["long"].replace("\n", "\\n")
    detail += (
        '            -   class: "UIControl"\n'
        '                name: "DetailDescription"\n'
        '                size: [880.000000, 240.000000]\n'
        '                input: false\n'
        '                classes: "t-body regular align-left white-wild-sand-70-text"\n'
        '                components:\n'
        '                    UITextComponent:\n'
        '                        colorInheritType: "COLOR_IGNORE_PARENT"\n'
        '                        multiline: "MULTILINE_ENABLED"\n'
        '                    Anchor:\n'
        '                        leftAnchorEnabled: true\n'
        '                        topAnchorEnabled: true\n'
        '                        topAnchor: 176.000000\n'
        '                    SizePolicy:\n'
        '                        horizontalPolicy: "FixedSize"\n'
        '                        horizontalValue: 880.000000\n'
        '                        verticalPolicy: "FixedSize"\n'
        '                        verticalValue: 240.000000\n'
        '                bindings:\n'
        f'                - ["UITextComponent.text", "\\"{esc(long_text)}\\""]\n'
    )
    # Resource patches are applied to files the client reads at startup, so a
    # button press cannot take effect in the running session. Saying so is the
    # difference between a working feature and one that looks broken.
    detail += (
        '            -   class: "UIControl"\n'
        '                name: "RestartHint"\n'
        '                size: [880.000000, 28.000000]\n'
        '                input: false\n'
        '                classes: "t-caption regular align-left orange-tango-text"\n'
        '                components:\n'
        '                    UITextComponent:\n'
        '                        colorInheritType: "COLOR_IGNORE_PARENT"\n'
        '                        multiline: "MULTILINE_DISABLED"\n'
        '                    Anchor:\n'
        '                        leftAnchorEnabled: true\n'
        '                        leftAnchor: 168.000000\n'
        '                        topAnchorEnabled: true\n'
        '                        topAnchor: 140.000000\n'
        '                    SizePolicy:\n'
        '                        horizontalPolicy: "FixedSize"\n'
        '                        horizontalValue: 880.000000\n'
        '                        verticalPolicy: "FixedSize"\n'
        '                        verticalValue: 28.000000\n'
        '                bindings:\n'
        '                - ["visible", "modRequestSent"]\n'
        '                - ["UITextComponent.text", '
        '"\\"Команда отправлена. Изменения применятся после перезапуска.\\""]\n'
    )
    return detail


def restart_bar() -> str:
    """Sits on the catalogue screen, not on a mod's page.

    A restart applies whatever has been queued, not one mod, so it belongs
    to the screen. The wrapper carries the visibility condition: it holds no
    UIDataParamsComponent, so the screen's variables are in scope there,
    while inside the button they would not be.
    """
    p = " " * 8
    wrapper = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "RestartBar"\n'
        f'{p}    size: [944.000000, 64.000000]\n'
        f'{p}    input: false\n'
        f'{p}    components:\n'
        f'{p}        IgnoreLayout: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            leftAnchor: 40.000000\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            rightAnchor: 40.000000\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: 96.000000\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: 64.000000\n'
        f'{p}    bindings:\n'
        f'{p}    - ["visible", "modRequestSent"]\n'
        f'{p}    children:\n'
    )
    return wrapper + restart_button(12)


def restart_button(indent: int) -> str:
    """Offered only once a request has been sent, since that is the only time
    restarting achieves anything."""
    p = " " * indent
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "RestartButton"\n'
        f'{p}    size: [260.000000, 52.000000]\n'
        f'{p}    classes: "simple-button orange-tango-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        UIInputEventComponent:\n'
        f'{p}            onTouchUpInside: "ON_CLICK"\n'
        f'{p}        UIDataParamsComponent:\n'
        f'{p}            events:\n'
        f'{p}            - "ON_CLICK"\n'
        f'{p}            eventActions:\n'
        f'{p}            - ["ON_CLICK", "ON_MOD_RESTART_CLICKED", ""]\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            bottomAnchorEnabled: true\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "PercentOfParent"\n'
        f'{p}    children:\n'
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "Caption"\n'
        f'{p}        input: false\n'
        f'{p}        classes: "t-button bold white-wild-sand-text"\n'
        f'{p}        components:\n'
        f'{p}            UITextComponent:\n'
        f'{p}                colorInheritType: "COLOR_IGNORE_PARENT"\n'
        f'{p}                multiline: "MULTILINE_DISABLED"\n'
        f'{p}                align: ["HCENTER", "VCENTER"]\n'
        f'{p}            Anchor:\n'
        f'{p}                leftAnchorEnabled: true\n'
        f'{p}                rightAnchorEnabled: true\n'
        f'{p}                topAnchorEnabled: true\n'
        f'{p}                bottomAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "PercentOfParent"\n'
        f'{p}                verticalPolicy: "PercentOfParent"\n'
        f'{p}        bindings:\n'
        f'{p}        - ["UITextComponent.text", "\\"ПЕРЕЗАПУСТИТЬ ИГРУ И ПРИМЕНИТЬ\\""]\n'
    )


def confirm_overlay() -> str:
    """A full-screen confirmation for removal.

    Placed last among the screen's children so it draws over the pages, and it
    takes input so a stray tap cannot reach the list behind it.
    """
    def button(name, caption, action, left, tint):
        p = " " * 16
        return (
            f'{p}-   class: "UIControl"\n'
            f'{p}    name: "{name}"\n'
            f'{p}    size: [200.000000, 52.000000]\n'
            f'{p}    classes: "simple-button {tint}"\n'
            f'{p}    components:\n'
            f'{p}        Background: {{}}\n'
            f'{p}        UIOpacityComponent: {{}}\n'
            f'{p}        UIInputEventComponent:\n'
            f'{p}            onTouchUpInside: "ON_CLICK"\n'
            f'{p}        UIDataParamsComponent:\n'
            f'{p}            events:\n'
            f'{p}            - "ON_CLICK"\n'
            f'{p}            eventActions:\n'
            f'{p}            - ["ON_CLICK", "{action}", ""]\n'
            f'{p}        Anchor:\n'
            f'{p}            leftAnchorEnabled: true\n'
            f'{p}            leftAnchor: {left:.6f}\n'
            f'{p}            bottomAnchorEnabled: true\n'
            f'{p}            bottomAnchor: 40.000000\n'
            f'{p}        SizePolicy:\n'
            f'{p}            horizontalPolicy: "FixedSize"\n'
            f'{p}            horizontalValue: 200.000000\n'
            f'{p}            verticalPolicy: "FixedSize"\n'
            f'{p}            verticalValue: 52.000000\n'
            f'{p}    children:\n'
            f'{p}    -   class: "UIControl"\n'
            f'{p}        name: "Caption"\n'
            f'{p}        input: false\n'
            f'{p}        classes: "t-button bold white-wild-sand-text"\n'
            f'{p}        components:\n'
            f'{p}            UITextComponent:\n'
            f'{p}                colorInheritType: "COLOR_IGNORE_PARENT"\n'
            f'{p}                multiline: "MULTILINE_DISABLED"\n'
            f'{p}                align: ["HCENTER", "VCENTER"]\n'
            f'{p}            Anchor:\n'
            f'{p}                leftAnchorEnabled: true\n'
            f'{p}                rightAnchorEnabled: true\n'
            f'{p}                topAnchorEnabled: true\n'
            f'{p}                bottomAnchorEnabled: true\n'
            f'{p}            SizePolicy:\n'
            f'{p}                horizontalPolicy: "PercentOfParent"\n'
            f'{p}                verticalPolicy: "PercentOfParent"\n'
            f'{p}        bindings:\n'
            f'{p}        - ["UITextComponent.text", "\\"{caption}\\""]\n'
        )

    head = '''        -   class: "UIControl"
            name: "ConfirmOverlay"
            size: [1024.000000, 768.000000]
            input: true
            components:
                Background:
                    drawType: "DRAW_FILL"
                    color: [0.020000, 0.030000, 0.040000, 0.880000]
                IgnoreLayout: {}
                Anchor:
                    leftAnchorEnabled: true
                    rightAnchorEnabled: true
                    topAnchorEnabled: true
                    bottomAnchorEnabled: true
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "PercentOfParent"
            bindings:
            - ["visible", "modConfirmVisible"]
            children:
            -   class: "UIControl"
                name: "Dialog"
                size: [520.000000, 220.000000]
                input: false
                classes: "grey-shark-80-bg"
                components:
                    Background: {}
                    Anchor:
                        hCenterAnchorEnabled: true
                        vCenterAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 520.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 220.000000
                children:
'''
    head += text_control("Title", "t-subtitle bold align-parent-center white-wild-sand-text",
                         0, 40, 520, 34, "Удалить мод?", 16)
    head += text_control("Body", "t-body regular align-parent-center white-wild-sand-70-text",
                         0, 82, 520, 26, "Файлы клиента вернутся к исходным.", 16)
    head += button("ConfirmRemove", "УДАЛИТЬ", "ON_MOD_REMOVE_CONFIRMED", 40.0,
                   "red-tamarillo-bg")
    head += button("CancelRemove", "ОТМЕНА", "ON_MOD_REMOVE_CANCELLED", 280.0,
                   "grey-shark-60-bg")
    return head


# ------------------------------------------------------------- hiding the UI

# Container draws its children in document order and FadedBlur covers only what
# precedes it, so these two are the only stock controls that survive the blur
# and land on top of the catalog. Everything above FadedBlur needs no help.
HIDE_WHEN_OPEN = ("TanksPanelHolder", "SideBar")


def add_visible_binding(text: str, name: str, expr: str) -> str:
    """Bind `visible` on a stock control, leaving its other keys untouched.

    Placed after `components:` and before `children:`, matching how the game
    writes its own controls. If the control already binds `visible`, the two
    expressions are combined rather than a second, conflicting entry added.
    """
    marker = f'name: "{name}"'
    if text.count(marker) != 1:
        raise SystemExit(f"expected exactly one {name!r}, found {text.count(marker)}")

    lines = text.splitlines(keepends=True)
    i = next(n for n, l in enumerate(lines) if marker in l)
    indent = len(lines[i]) - len(lines[i].lstrip(" "))
    pad = " " * indent

    j = i + 1
    while j < len(lines):
        line = lines[j]
        if not line.strip():
            j += 1
            continue
        ind = len(line) - len(line.lstrip(" "))
        if ind < indent:                     # this control ended
            break
        if ind == indent:
            key = line.strip()
            if key.startswith("bindings:"):
                k = j + 1
                while k < len(lines) and lines[k].strip().startswith("-"):
                    if '"visible"' in lines[k]:
                        old = lines[k].split('", "', 1)[1].rsplit('"]', 1)[0]
                        lines[k] = f'{pad}- ["visible", "({old}) and {expr}"]\n'
                        return "".join(lines)
                    k += 1
                lines.insert(k, f'{pad}- ["visible", "{expr}"]\n')
                return "".join(lines)
            if key.startswith("children:"):
                break
        j += 1

    lines.insert(j, f'{pad}bindings:\n{pad}- ["visible", "{expr}"]\n')
    return "".join(lines)


# ---------------------------------------------------------------- validation

def _logical(line: str):
    """(indent, text, starts_item) with a leading `-` normalised to spaces.

    `-   class: "UIControl"` at column 4 is logically a key at column 8, so
    treating the dash as indentation makes list items and plain keys directly
    comparable.
    """
    indent = len(line) - len(line.lstrip(" "))
    body = line[indent:]
    if body.startswith("-"):
        rest = body[1:]
        pad = len(rest) - len(rest.lstrip(" "))
        return indent + 1 + pad, rest.lstrip(" "), True
    return indent, body, False


def validate(text: str) -> None:
    """Catch structural damage before it reaches the game.

    DAVA gives no parse diagnostics — a malformed screen is a crash on load,
    so the two failure modes we have actually produced are checked here:
    an indent that rises after a line that opened nothing, and a key repeated
    inside one mapping.
    """
    errors = []
    stack: list[tuple[int, set]] = []
    prev_indent, prev_opens = 0, True

    for n, raw in enumerate(text.splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent, body, is_item = _logical(raw)

        if indent > prev_indent and not prev_opens:
            errors.append(f"line {n}: indent rises after a line that opens no "
                          f"block -> {raw.strip()[:60]!r}")

        while stack and stack[-1][0] > indent:
            stack.pop()
        if not stack or stack[-1][0] < indent:
            stack.append((indent, set()))
        if is_item:                       # a new sibling mapping starts here
            stack[-1] = (indent, set())

        key = body.split(":", 1)[0].strip() if ":" in body else None
        if key and not key.startswith(("[", '"[')):
            if key in stack[-1][1]:
                errors.append(f"line {n}: duplicate key {key!r} in this mapping")
            stack[-1][1].add(key)

        prev_indent = indent
        prev_opens = body.rstrip().endswith(":") or is_item

    if errors:
        raise SystemExit("generated YAML is structurally invalid:\n  "
                         + "\n  ".join(errors))


# ------------------------------------------------------------------- splice

LOCALS_ANCHOR = '            - ["bool", "showXpBonusHint", "false"]\n'
BUTTON_ANCHOR = ('                                                    children:\n'
                 '                                                    -   class: "UIControl"\n'
                 '                                                        name: "StoryAggregatedButtonsHolder"\n')

BUTTON_BLOCK = '''                                                    children:
                                                    -   prototype: "IconButtonWithBadge/IconButton"
                                                        name: "ModCatalogButton"
                                                        components:
                                                            UIDataParamsComponent:
                                                                args:
                                                                    "image": "\\"~res:/Gfx/Lobby/icons/icon_settings_n\\""
                                                                    "type": "eButtonType.OPTIONAL_LIGHT"
                                                                    "visible": "true"
                                                                eventActions:
                                                                - ["ON_CLICK_BUTTON", "ON_MOD_CATALOG_CLICKED", ""]
                                                    -   class: "UIControl"
                                                        name: "StoryAggregatedButtonsHolder"
'''


ACTIONS_REL = "UI/Screens3/Lobby/Hangar/Hangar.actions"

# Generated alongside the screen: the card indices the actions receive come
# from the same enumeration that lays the cards out, so the two files cannot
# be allowed to drift apart.
ACTIONS_BLOCK = '''
action ON_MOD_CATALOG_CLICKED()
{
  if (modCatalogVisible)
  {
    ChangeData(modCatalogVisible, false);
    ChangeData(modDetailVisible, false);
    Opacity("**/FadedBlur", 0.0, time=0.2, interpolation=EASE_OUT);
    Event("ENABLE_BLUR", arg1=false);
    Event("PAUSE_HANGAR_SCENE", arg1=false);
  }
  else
  {
    ChangeData(modCatalogVisible, true);
    ChangeData(modDetailVisible, false);
    ChangeData(modRequestSent, false);
    Event("PAUSE_HANGAR_SCENE", arg1=true);
    // Same blur the stock screens fade the hangar out with.
    RenderPostProcess("**/FadedBlur/BlurAndFade/Blur", force=true);
    Event("ENABLE_BLUR", arg1=true);
    Opacity("**/FadedBlur", 1.0, time=0.2, interpolation=EASE_OUT);
  }
}

// Back steps out of the detail page first, and only closes the whole screen
// once the list is what is on show.
action ON_MOD_CATALOG_BACK()
{
  if (modDetailVisible)
  {
    ChangeData(modDetailVisible, false);
    ChangeData(modRequestSent, false);
  }
  else
  {
    Event("ON_MOD_CATALOG_CLICKED");
  }
}

action ON_MOD_CARD_CLICKED(int index)
{
  PlaySound(sound="GUI/buttons/open");
  ChangeData(modDetailIndex, index);
  ChangeData(modRequestSent, false);
  ChangeData(modDetailVisible, true);
}

// The actions language has no file or network access - its whole vocabulary is
// UI and animation - so a button cannot run the installer itself. What it can
// do is write to the client log, which agent.py tails and acts on. The index
// is resolved through cache/catalog_index.json, written by the same run that
// laid these cards out, so the two cannot disagree about which mod is which.
// A request is a sprite the client cannot find. Asking for
// ~res:/BLITZFORGE/<verb>/<index>-<seq> makes the engine log
//   [error] [ConvertedFileSpriteDataLoader] File "..." not found
// and error level does reach blitz-logs_*.txt, where agent.py reads it. The
// sequence number keeps every press a distinct path, so a repeated action is
// not swallowed by the failed-sprite cache.
//
// Log() is emitted too, in case that channel turns out to work: it is the
// cheaper of the two and costs nothing to try.
// Which action a button carries is fixed when the screen is built, from the
// ledger, but the button flips as soon as it is pressed - so a second press
// has to mean the opposite of the first. modPendingInstalled holds the state
// the pending request will produce, and these two branches read it back.
//
// This one is attached to a mod that was not installed when the screen was
// built, so it reads УСТАНОВИТЬ until pressed.
action ON_MOD_INSTALL_CLICKED(int index)
{
  if (modRequestSent and modRequestIndex == index and modPendingInstalled)
  {
    ChangeData(modConfirmVisible, true);
  }
  else
  {
    PlaySound(sound="GUI/buttons/open");
    Log("BLITZFORGE:install:" + str(index));
    ChangeData(modRequestVerb, 1);
    ChangeData(modRequestIndex, index);
    ChangeData(modPendingInstalled, true);
    ChangeData(modRequestSeq, modRequestSeq + 1);
    ChangeData(modRequestSent, true);
  }
}

// Attached to a mod that was installed when the screen was built. Removal asks
// first; installing back does not need a confirmation.
action ON_MOD_REMOVE_CLICKED(int index)
{
  if (modRequestSent and modRequestIndex == index and not modPendingInstalled)
  {
    PlaySound(sound="GUI/buttons/open");
    Log("BLITZFORGE:install:" + str(index));
    ChangeData(modRequestVerb, 1);
    ChangeData(modPendingInstalled, true);
    ChangeData(modRequestSeq, modRequestSeq + 1);
    ChangeData(modRequestSent, true);
  }
  else
  {
    ChangeData(modRequestIndex, index);
    ChangeData(modConfirmVisible, true);
  }
}

action ON_MOD_REMOVE_CONFIRMED()
{
  PlaySound(sound="GUI/buttons/open");
  Log("BLITZFORGE:remove:" + str(modRequestIndex));
  ChangeData(modConfirmVisible, false);
  ChangeData(modRequestVerb, 2);
  ChangeData(modPendingInstalled, false);
  ChangeData(modRequestSeq, modRequestSeq + 1);
  ChangeData(modRequestSent, true);
}

action ON_MOD_REMOVE_CANCELLED()
{
  PlaySound(sound="GUI/buttons/close");
  ChangeData(modConfirmVisible, false);
}

action ON_MOD_RESTART_CLICKED()
{
  PlaySound(sound="GUI/buttons/open");
  Log("BLITZFORGE:restart:0");
  ChangeData(modRequestVerb, 3);
  ChangeData(modRequestSeq, modRequestSeq + 1);
  ChangeData(modRequestSent, true);
}
'''


def rebuild_actions() -> None:
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "extract", ACTIONS_REL],
                   check=True)
    src = WORK / "Hangar.actions"
    text = src.read_text(encoding="utf-8")
    if "ON_MOD_CATALOG_CLICKED" in text:
        raise SystemExit("extract did not return a pristine Hangar.actions")
    src.write_text(text.rstrip() + "\n" + ACTIONS_BLOCK, encoding="utf-8")
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "install", ACTIONS_REL, str(src)],
                   check=True)


def rebuild(dry_run: bool = False, source: str = "registry") -> None:
    mods = load_mods(source)
    print(f"mods: {len(mods)}")
    for m in mods:
        print(f"   {m['id']:16} {m['name']}  ({m['type']})")

    screen = build_screen(mods)
    if dry_run:
        print(screen)
        return

    # agent.py sets this before relaunching, so the catalogue is already open
    # when the hangar comes back. Consuming the flag here stops it reopening on
    # every launch from then on.
    flag = HERE / "cache" / "open_catalog_on_load"
    open_on_load = "true" if flag.exists() else "false"
    if flag.exists():
        flag.unlink()
        print("catalog will open on load (requested by a restart)")

    # start from the pristine screen every time
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "extract", HANGAR_REL],
                   check=True)
    src = WORK / "Hangar.yaml"
    text = src.read_text(encoding="utf-8", errors="replace")
    if "ModCatalog" in text:
        raise SystemExit("extract did not return a pristine Hangar.yaml")

    for anchor in (LOCALS_ANCHOR, BUTTON_ANCHOR, "Slots:"):
        if anchor not in text:
            raise SystemExit(f"anchor missing from Hangar.yaml: {anchor[:60]!r}")

    text = text.replace(
        LOCALS_ANCHOR,
        LOCALS_ANCHOR
        + f'            - ["bool", "modCatalogVisible", "{open_on_load}"]\n'
        + '            - ["bool", "modDetailVisible", "false"]\n'
        + '            - ["int", "modDetailIndex", "0"]\n'
        + '            - ["bool", "modRequestSent", "false"]\n'
        + '            - ["bool", "modConfirmVisible", "false"]\n'
        + '            - ["int", "modRequestVerb", "0"]\n'
        + '            - ["int", "modRequestIndex", "0"]\n'
        + '            - ["int", "modRequestSeq", "0"]\n', 1)
    text = text.replace(BUTTON_ANCHOR, BUTTON_BLOCK, 1)

    for name in HIDE_WHEN_OPEN:
        text = add_visible_binding(text, name, "not modCatalogVisible")

    text = text.replace("Slots:", screen + "Slots:", 1)

    # The card order is the contract between the buttons and agent.py: the
    # button only knows its index, and this is what turns it back into an id.
    (HERE / "cache").mkdir(parents=True, exist_ok=True)
    (HERE / "cache" / "catalog_index.json").write_text(
        json.dumps({"order": [m["id"] for m in mods]}, indent=2), encoding="utf-8")

    validate(text)          # never install a screen the game would die on
    src.write_text(text, encoding="utf-8")
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "install", HANGAR_REL, str(src)],
                   check=True)
    rebuild_actions()

    # The screen is useless without something to carry out what it asks for.
    import agent
    agent.ensure_autostart()

    print(f"catalog rebuilt with {len(mods)} mod(s)")


if __name__ == "__main__":
    rebuild(dry_run="--dry-run" in sys.argv,
            source="local" if "--local" in sys.argv else "registry")
