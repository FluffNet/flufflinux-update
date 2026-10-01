//! First QML-facing state migrated without changing the KCM/QML contract.
//! The KDE adapter handles QSettings I/O; this QObject owns the state and emits
//! the native Qt change notifications consumed through the existing KCM.

use std::pin::Pin;

#[cxx_qt::bridge(namespace = "flu")]
mod ffi {
    extern "RustQt" {
        #[qobject]
        #[qproperty(bool, pacman_view, cxx_name = "pacmanView")]
        #[qproperty(i32, window_width, cxx_name = "windowWidth")]
        #[qproperty(i32, window_height, cxx_name = "windowHeight")]
        #[qproperty(bool, window_maximized, cxx_name = "windowMaximized")]
        type UiState = super::UiStateRust;

        #[qinvokable]
        #[cxx_name = "saveWindowState"]
        fn save_window_state(self: Pin<&mut UiState>, width: i32, height: i32, maximized: bool);
    }
}

#[derive(Default)]
pub struct UiStateRust {
    pacman_view: bool,
    window_width: i32,
    window_height: i32,
    window_maximized: bool,
}

impl ffi::UiState {
    fn save_window_state(mut self: Pin<&mut Self>, width: i32, height: i32, maximized: bool) {
        self.as_mut().set_window_width(width.max(480));
        self.as_mut().set_window_height(height.max(400));
        self.as_mut().set_window_maximized(maximized);
    }
}
