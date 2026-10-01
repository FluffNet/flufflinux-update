// Keep the Qt runtime crates linked even before the QML QObject is migrated.
// CXX-Qt's CMake initializer references their native initialization symbols.
use cxx_qt as _;
use cxx_qt_lib as _;

mod recovery;
mod ui_state;
