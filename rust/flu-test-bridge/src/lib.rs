//! Native regression-test adapters; never installed or linked into FLU.
// CMake's generated initializer references the Qt runtime symbols even though
// this adapter exposes only plain CXX types. These linkage imports are needed.
use cxx_qt as _;
use cxx_qt_lib as _;
mod recovery;
