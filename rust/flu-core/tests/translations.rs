//! Validate installed-format translations and desktop integration metadata.
use regex::Regex;
use serde_json::Value;
use std::{collections::BTreeMap, fs, path::PathBuf, process::Command};

// English is the source language. The manifest is the complete translation target,
// not a scan of whichever catalogs happen to exist in the checkout.
fn languages() -> Vec<String> {
    let manifest: Value =
        serde_json::from_str(&fs::read_to_string(root().join("po/languages.json")).unwrap())
            .unwrap();
    let choices = manifest["languages"].as_array().unwrap();
    assert_eq!(choices.len(), 96);
    assert_eq!(
        choices
            .iter()
            .map(|v| v["locales"].as_array().unwrap().len())
            .sum::<usize>(),
        172
    );
    let mut codes: Vec<String> = choices
        .iter()
        .map(|v| v["code"].as_str().unwrap())
        .filter(|code| *code != "en")
        .chain(
            manifest["regional_catalogs"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_str().unwrap()),
        )
        .map(str::to_owned)
        .collect();
    codes.sort();
    codes.dedup();
    assert_eq!(codes.len(), 97);
    codes
}

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn placeholders(text: &str) -> Vec<String> {
    let pattern = Regex::new(r"%[1-9][0-9]*").unwrap();
    let mut result: Vec<_> = pattern
        .find_iter(text)
        .map(|m| m.as_str().to_owned())
        .collect();
    result.sort();
    result
}

fn messages(source: &str) -> Vec<String> {
    source
        .lines()
        .filter_map(|line| line.strip_prefix("msgid "))
        .map(|quoted| serde_json::from_str::<String>(quoted).unwrap())
        .filter(|message| !message.is_empty())
        .collect()
}

// Inspect the actual MO payload so fuzzy or omitted entries cannot pass.
fn read_mo(bytes: &[u8]) -> BTreeMap<String, String> {
    let little = match &bytes[..4] {
        [0xde, 0x12, 0x04, 0x95] => true,
        [0x95, 0x04, 0x12, 0xde] => false,
        _ => panic!("Invalid gettext catalog"),
    };
    let word = |offset: usize| {
        let value: [u8; 4] = bytes[offset..offset + 4].try_into().unwrap();
        (if little {
            u32::from_le_bytes(value)
        } else {
            u32::from_be_bytes(value)
        }) as usize
    };
    let field = |table: usize, index: usize| {
        let entry = table + index * 8;
        let (length, offset) = (word(entry), word(entry + 4));
        String::from_utf8(bytes[offset..offset + length].to_vec()).unwrap()
    };
    (0..word(8))
        .map(|i| (field(word(12), i), field(word(16), i)))
        .collect()
}

#[test]
fn every_catalog_compiles_and_translates_every_message_and_plural() {
    let root = root();
    let expected = messages(&fs::read_to_string(root.join("po/kcm_fluffupdates.pot")).unwrap());
    let mut actual: Vec<_> = fs::read_dir(root.join("po"))
        .unwrap()
        .map(Result::unwrap)
        .filter(|e| e.path().join("kcm_fluffupdates.po").is_file())
        .map(|e| e.file_name().into_string().unwrap())
        .collect();
    actual.sort();
    assert_eq!(actual, languages());
    let scratch = tempfile::tempdir().unwrap();
    let plural_count = Regex::new(r"nplurals=(\d+)").unwrap();
    for language in languages() {
        let output = scratch.path().join(format!("{language}.mo"));
        let result = Command::new(std::env::var_os("MSGFMT").unwrap_or_else(|| "msgfmt".into()))
            .args(["--check", "--check-format", "-o"])
            .arg(&output)
            .arg(root.join(format!("po/{language}/kcm_fluffupdates.po")))
            .output()
            .expect("Install gettext to run translation tests");
        assert!(
            result.status.success(),
            "{language}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        let catalog = read_mo(&fs::read(output).unwrap());
        let header = catalog.get("").unwrap();
        assert!(
            header
                .lines()
                .any(|line| line == format!("Language: {language}"))
        );
        let count = plural_count.captures(header).unwrap()[1]
            .parse::<usize>()
            .unwrap();
        let by_singular: BTreeMap<_, _> = catalog
            .iter()
            .filter(|(key, _)| !key.is_empty())
            .map(|(key, value)| (key.split('\0').next().unwrap(), (key, value)))
            .collect();
        assert_eq!(
            by_singular.len(),
            expected.len(),
            "{language}: catalog/template mismatch"
        );
        for message in &expected {
            let (key, translated) = by_singular
                .get(message.as_str())
                .unwrap_or_else(|| panic!("{language}: missing translation for {message}"));
            let forms: Vec<_> = translated.split('\0').collect();
            assert_eq!(
                forms.len(),
                if key.contains('\0') { count } else { 1 },
                "{language}: {message}"
            );
            for (index, form) in forms.into_iter().enumerate() {
                assert!(
                    !form.trim().is_empty(),
                    "{language}: empty translation for {message}"
                );
                // Existing Arabic zero/singular/dual and Hebrew singular/dual
                // phrases express their counts in words rather than numerals.
                let implicit_count = key.contains('\0')
                    && ((language == "ar" && index <= 2) || (language == "he" && index <= 1))
                    && placeholders(form).is_empty();
                if !implicit_count {
                    assert_eq!(
                        placeholders(form),
                        placeholders(message),
                        "{language}: {message}"
                    );
                }
                assert!(
                    !form.contains(['\u{2013}', '\u{2014}']),
                    "{language}: use regular hyphens: {message}"
                );
            }
        }
    }
}

#[test]
fn every_language_has_launcher_settings_and_authorization_translations() {
    let root = root();
    let metadata: Value =
        serde_json::from_str(&fs::read_to_string(root.join("src/kcm_flu.json")).unwrap()).unwrap();
    let desktop = fs::read_to_string(root.join("src/org.flufflinux.update.desktop")).unwrap();
    let policy = fs::read_to_string(root.join("src/org.flufflinux.update.policy")).unwrap();
    let manifest: Value =
        serde_json::from_str(&fs::read_to_string(root.join("po/languages.json")).unwrap()).unwrap();
    let mut metadata_languages = languages();
    metadata_languages.extend(manifest["aliases"].as_object().unwrap().keys().cloned());
    metadata_languages.sort();
    metadata_languages.dedup();
    for language in metadata_languages {
        for field in ["Name", "Description"] {
            assert!(
                !metadata["KPlugin"][format!("{field}[{language}]")]
                    .as_str()
                    .unwrap()
                    .trim()
                    .is_empty()
            );
        }
        assert!(
            !metadata[format!("X-KDE-Keywords[{language}]")]
                .as_str()
                .unwrap()
                .trim()
                .is_empty()
        );
        for field in ["Name", "Comment", "Keywords"] {
            let prefix = format!("{field}[{language}]=");
            let values: Vec<_> = desktop
                .lines()
                .filter_map(|line| line.strip_prefix(&prefix))
                .collect();
            assert_eq!(values.len(), 1, "{language}: {field}");
            assert!(!values[0].trim().is_empty());
        }
        for field in ["description", "message"] {
            let pattern = Regex::new(&format!(
                r#"<{field} xml:lang="{language}">([^<]+)</{field}>"#
            ))
            .unwrap();
            assert_eq!(
                pattern.captures_iter(&policy).count(),
                1,
                "{language}: {field}"
            );
        }
    }
    let separated_prefix =
        Regex::new(r"(?:^|[^\p{Hebrew}])[בכלמשה][\s־-]+(?:Fluff Linux|GitHub|Pacman)\b").unwrap();
    let hebrew = fs::read_to_string(root.join("po/he/kcm_fluffupdates.po")).unwrap();
    assert!(!separated_prefix.is_match(&hebrew));
    assert!(!separated_prefix.is_match(&policy));
}

#[test]
fn regional_locales_keep_the_selected_script_and_complete_catalog() {
    let root = root();
    let manifest: Value =
        serde_json::from_str(&fs::read_to_string(root.join("po/languages.json")).unwrap()).unwrap();
    let catalogs = languages();
    let aliases = manifest["aliases"].as_object().unwrap();
    for (alias, target) in aliases {
        let target = target.as_str().unwrap();
        assert!(
            catalogs.iter().any(|code| code == target),
            "{alias}: {target}"
        );
        assert!(
            alias
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || "_@".contains(c))
        );
    }
    let mut locales = std::collections::BTreeSet::new();
    let mut rtl = Vec::new();
    for language in manifest["languages"].as_array().unwrap() {
        let code = language["code"].as_str().unwrap();
        let regions = language["locales"].as_array().unwrap();
        assert!(
            regions.contains(&language["locale"]),
            "{code}: missing default"
        );
        assert!(!language["native"].as_str().unwrap().is_empty());
        if language["rtl"].as_bool().unwrap() {
            rtl.push(code);
        }
        for region in regions {
            let region = region.as_str().unwrap();
            assert!(locales.insert(region), "Duplicate locale: {region}");
            // Remove the encoding only, never the script modifier after it.
            let normalized = region.replace(".UTF-8", "");
            let resolved = aliases
                .get(&normalized)
                .and_then(Value::as_str)
                .unwrap_or_else(|| normalized.split('_').next().unwrap());
            assert!(
                resolved == code
                    || manifest["regional_catalogs"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .any(|v| v.as_str() == Some(resolved)),
                "{region}: {resolved} != {code}"
            );
        }
    }
    assert_eq!(locales.len(), 172);
    rtl.sort();
    assert_eq!(rtl, ["ar", "fa", "he", "pa_Arab", "ps", "ug", "ur"]);
    for (locale, target) in [
        ("zh_CN", "zh_Hans"),
        ("zh_SG", "zh_Hans"),
        ("zh_TW", "zh_Hant"),
        ("zh_HK", "zh_Hant"),
        ("sr_RS@latin", "sr_Latn"),
        ("ca_ES@valencia", "ca_valencia"),
        ("pa_PK", "pa_Arab"),
        ("pa_IN", "pa"),
    ] {
        assert_eq!(aliases[locale], target);
    }
    let swiss = fs::read_to_string(root.join("po/de_CH/kcm_fluffupdates.po")).unwrap();
    assert!(!swiss.contains(['ß', 'ẞ']));
    let latin = fs::read_to_string(root.join("po/sr_Latn/kcm_fluffupdates.po")).unwrap();
    assert!(!Regex::new(r"\p{Cyrillic}").unwrap().is_match(&latin));
    for (code, script) in [("pa", r"\p{Gurmukhi}"), ("pa_Arab", r"\p{Arabic}")] {
        let source =
            fs::read_to_string(root.join(format!("po/{code}/kcm_fluffupdates.po"))).unwrap();
        assert!(Regex::new(script).unwrap().is_match(&source));
    }
}

#[test]
fn qml_and_notification_messages_are_in_the_template() {
    let root = root();
    let template = messages(&fs::read_to_string(root.join("po/kcm_fluffupdates.pot")).unwrap());
    let qml = fs::read_to_string(root.join("src/ui/main.qml")).unwrap();
    let pattern = Regex::new(r#"i18nd\("kcm_fluffupdates",\s*("(?:[^"\\]|\\.)*")"#).unwrap();
    for capture in pattern.captures_iter(&qml) {
        let message: String = serde_json::from_str(&capture[1]).unwrap();
        assert!(
            template.contains(&message),
            "QML message missing from template: {message}"
        );
    }
    for message in flu_core::notifications::messages() {
        assert!(
            template.iter().any(|item| item == message),
            "Notification missing from template: {message}"
        );
    }
}
