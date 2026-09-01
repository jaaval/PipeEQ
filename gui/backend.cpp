#include "backend.h"

namespace pipeeq {

// Out of line on purpose: an abstract QObject still needs a translation unit
// for its vtable and its moc-generated meta-object to land in. Without this
// file, AUTOMOC never processes backend.h at all - headers are only scanned
// when a matching source file exists - and every subclass fails to link
// against a missing Backend::staticMetaObject.
Backend::Backend(QObject* parent) : QObject(parent) {}

Backend::~Backend() = default;

} // namespace pipeeq
