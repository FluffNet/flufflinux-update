//! Unprivileged, D-Bus-activated notification observer; no package operations.
fn main() {
    if unsafe { libc::geteuid() } == 0 {
        eprintln!("FLU notifications must run in the regular user's session.");
        std::process::exit(1);
    }
    if let Err(error) = flu_core::notifications::run(localized_messages()) {
        eprintln!("FLU notifications: {error}");
        std::process::exit(1);
    }
}

// Use the system gettext catalogs before starting any threads. The GUI can
// later supply its own language selection through the existing session API.
fn localized_messages() -> std::collections::HashMap<String, String> {
    #[cfg(target_os = "linux")]
    {
        use std::ffi::{CStr, CString};
        unsafe extern "C" {
            fn bindtextdomain(
                domain: *const libc::c_char,
                dir: *const libc::c_char,
            ) -> *mut libc::c_char;
            fn bind_textdomain_codeset(
                domain: *const libc::c_char,
                code: *const libc::c_char,
            ) -> *mut libc::c_char;
            fn dgettext(
                domain: *const libc::c_char,
                message: *const libc::c_char,
            ) -> *mut libc::c_char;
        }
        let domain = c"kcm_fluffupdates";
        // SAFETY: initialization is single-threaded and all arguments are
        // valid NUL-terminated strings. gettext owns its returned pointers.
        unsafe {
            libc::setlocale(libc::LC_ALL, c"".as_ptr());
            bindtextdomain(domain.as_ptr(), c"/usr/share/locale".as_ptr());
            bind_textdomain_codeset(domain.as_ptr(), c"UTF-8".as_ptr());
        }
        flu_core::notifications::messages()
            .map(|key| {
                let message = CString::new(key).expect("static translation key");
                let value = unsafe { CStr::from_ptr(dgettext(domain.as_ptr(), message.as_ptr())) };
                (key.to_owned(), value.to_string_lossy().into_owned())
            })
            .collect()
    }
    #[cfg(not(target_os = "linux"))]
    std::collections::HashMap::new()
}
