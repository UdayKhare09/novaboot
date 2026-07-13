#pragma once

/// @file novaboot/di/di.h
/// Umbrella header for the NovaBoot DI/IoC sub-library.
///
/// Include this single header to get the explicit DI API:
///
///   #include "novaboot/di/di.h"
///
/// Register dependencies at the composition root:
///
///   root.singleton<UserRepository>([](auto&) { return new UserRepository(); });
///   root.singleton<UserService>([](auto& c) {
///       return new UserService(c.template resolve<UserRepository>());
///   });

#include "novaboot/di/scope.h"
#include "novaboot/di/lifecycle.h"
#include "novaboot/di/inject.h"
#include "novaboot/di/container.h"
#include "novaboot/di/handler_injector.h"
