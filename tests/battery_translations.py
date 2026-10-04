#!/usr/bin/env python3
"""Verify battery and every tray-notification lookup in compiled catalogs."""
import argparse
from collections import Counter
import gettext
import json
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

WARNING = (
    "The battery is low. Please connect your computer to a charger "
    "while installing updates."
)
OLD_WARNING = "The battery is low. Please plug in your computer before installing updates."
LOCALES = set("ar bg cs da de el es et fi fr ga he hr hu it ja lt lv mt nl pl pt ro ru sk sl sv".split())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--msgfmt", required=True)
    args = parser.parse_args()
    root = args.source.resolve(strict=True)
    qml = (root / "src/ui/main.qml").read_text()
    template = (root / "po/kcm_fluffupdates.pot").read_text()
    assert WARNING in qml and OLD_WARNING not in qml
    assert template.count(f'msgid "{WARNING}"') == 1 and OLD_WARNING not in template
    catalogs = sorted((root / "po").glob("*/kcm_fluffupdates.po"))
    assert {po.parent.name for po in catalogs} == LOCALES
    notifier = (root / "rust/flu-core/src/notifications.rs").read_text()
    messages = notifier.split("pub const MESSAGES: &[&str] = &[", 1)[1].split("];", 1)[0]
    notification_messages = [json.loads(value) for value in re.findall(r'"(?:[^"\\]|\\.)*"', messages)]
    assert len(notification_messages) >= 13
    operational = (root / "rust/flu-core/src/operational_error.rs").read_text()
    messages = operational.split("pub const MESSAGES: &[&str] = &[", 1)[1].split("];", 1)[0]
    notification_messages += [json.loads(value) for value in re.findall(r'"(?:[^"\\]|\\.)*"', messages)]
    cmake = (root / "src/CMakeLists.txt").read_text()
    for status, color in (("success", "#27ae60"), ("error", "#d71920")):
        name = f"flufflinux-update-{status}"
        assert f'"icons/{name}.svg"' in qml
        assert f'"{name}"' in notifier and f"ui/icons/{name}.svg" in cmake
        svg = ET.parse(root / f"src/ui/icons/{name}.svg").getroot()
        circle = svg.find("{http://www.w3.org/2000/svg}circle")
        glyph = svg.find("{http://www.w3.org/2000/svg}path")
        assert circle is not None and circle.attrib["fill"] == color
        assert glyph is not None and glyph.attrib["fill"] == "white"
    with tempfile.TemporaryDirectory(prefix="flu-battery-translations-") as scratch:
        for po in catalogs:
            locale = po.parent.name
            source = po.read_text()
            assert source.count(f'msgid "{WARNING}"') == 1, locale
            assert OLD_WARNING not in source, locale
            output = Path(scratch) / f"{locale}.mo"
            result = subprocess.run(
                [args.msgfmt, "--check", "--check-format", "-o", str(output), str(po)],
                text=True, capture_output=True,
            )
            assert result.returncode == 0, f"{locale}: {result.stderr}"
            with output.open("rb") as stream:
                catalog = gettext.GNUTranslations(stream)
            translated = catalog.gettext(WARNING)
            # msgfmt excludes fuzzy/empty entries, so this verifies the actual
            # installed-format lookup, not just the presence of a PO string.
            assert translated.strip() and translated != WARNING, locale
            for message in notification_messages:
                translated_message = catalog.gettext(message)
                assert message in catalog._catalog, (locale, message)
                assert translated_message.strip(), (locale, message)
                if message != "Fluff Linux Update":
                    assert translated_message != message, (locale, message)
                assert Counter(re.findall(r"%[1-9][0-9]*", translated_message)) == Counter(
                    re.findall(r"%[1-9][0-9]*", message)
                ), (locale, message, translated_message)
            if locale == "he":
                print(f"Hebrew: {translated}")
    print(f"PASS: battery warning and all {len(notification_messages)} tray messages "
          f"in all {len(catalogs)} compiled catalogs")


if __name__ == "__main__":
    main()
