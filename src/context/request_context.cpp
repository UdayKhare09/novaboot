// request_context.cpp
// The RequestContext implementation.
//
// inject<T>() and inject_named<T>() are defined inline in the header
// (after including novaboot/di/container.h) to avoid link-time
// template instantiation issues.
//
// This .cpp is kept for future non-template implementation additions.

#include "novaboot/context/request_context.h"

// Nothing to define here — all methods are either inline in the header
// or trivial enough to be header-only.
