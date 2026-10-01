#pragma once
// Generic Qt/KDE interoperability only; update decisions live in Rust.
#include <QString>
#include <QVariant>
#include <QList>
#include <cstdint>
#include "rust/cxx.h"
namespace flu {
::rust::String translate(::rust::Str message, ::rust::Str plural, std::int64_t count, const ::rust::Vec<::rust::String> &arguments);
::rust::String config_directory();
void initialize_locale(const ::rust::Vec<::rust::String> &languages);
QList<QVariant> variants(::rust::Str json);
void copy_text(const QString &text);
void open_url(::rust::Str url);
std::int32_t reachability();
::rust::Vec<std::uint32_t> interface_flags();
}
