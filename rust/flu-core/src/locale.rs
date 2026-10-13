//! Resolve interface direction from FLU's translated language, not the host UI.
use serde_json::Value;

pub fn is_right_to_left(preferences: &[String]) -> bool {
    let inventory: Value = serde_json::from_str(include_str!("../../../po/languages.json"))
        .expect("validated language inventory");
    let languages = inventory["languages"].as_array().unwrap();
    for preference in preferences {
        // The encoding precedes the modifier: sr_RS.UTF-8@latin.
        let (locale, modifier) = preference.split_once('@').unwrap_or((preference, ""));
        let mut name = locale.split('.').next().unwrap_or(locale).replace('-', "_");
        if !modifier.is_empty() {
            name.push('@');
            name.push_str(modifier);
        }
        let canonical = inventory["aliases"][&name].as_str().unwrap_or(&name);
        let canonical = if inventory["regional_catalogs"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v.as_str() == Some(canonical))
        {
            canonical.split('_').next().unwrap()
        } else {
            canonical
        };
        if let Some(language) = languages.iter().find(|language| {
            language["code"].as_str() == Some(canonical)
                || language["locales"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .any(|region| region.as_str().unwrap().replace(".UTF-8", "") == name)
        }) {
            return language["rtl"].as_bool().unwrap();
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn script_country_and_encoding_forms_keep_their_direction() {
        for locale in [
            "ar",
            "he_IL.UTF-8",
            "fa_IR",
            "ur_PK",
            "ps_AF",
            "ug_CN",
            "pa_Arab",
            "pa_PK.UTF-8",
            "pa-Arab-PK",
        ] {
            assert!(is_right_to_left(&[locale.into()]), "{locale}");
        }
        for locale in [
            "pa",
            "pa_IN.UTF-8",
            "pa_Guru",
            "sr_RS.UTF-8@latin",
            "zh-Hant",
            "ca_ES.UTF-8@valencia",
            "de_CH",
            "pt_BR",
            "ku_TR",
        ] {
            assert!(!is_right_to_left(&[locale.into()]), "{locale}");
        }
    }

    #[test]
    fn first_supported_translation_wins_without_cross_script_fallback() {
        assert!(!is_right_to_left(&["en_US".into(), "ar".into()]));
        assert!(!is_right_to_left(&["pt_BR".into(), "ar".into()]));
        assert!(is_right_to_left(&["unsupported".into(), "pa_PK".into()]));
        assert!(is_right_to_left(&["ku_Arab".into(), "ar".into()]));
        assert!(!is_right_to_left(&[]));
    }
}
