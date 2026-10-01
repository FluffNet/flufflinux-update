#!/usr/bin/env python3
"""Compile every shipped catalog and verify the new battery-warning lookup."""
import argparse
import gettext
from pathlib import Path
import subprocess
import tempfile

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
            if locale == "he":
                print(f"Hebrew: {translated}")
    print(f"PASS: updated battery warning in all {len(catalogs)} compiled catalogs")


if __name__ == "__main__":
    main()
