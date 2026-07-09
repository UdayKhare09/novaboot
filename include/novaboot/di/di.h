#pragma once

/// @file novaboot/di/di.h
/// Umbrella header for the NovaBoot DI/IoC sub-library.
///
/// REQUIRES: GCC 16+ or Clang 22+, -std=c++26, -freflection
///
/// Include this single header to get everything:
///
///   #include "novaboot/di/di.h"
///
/// Then annotate your beans:
///
///   struct [[=novaboot::di::component{}]] UserService {
///       explicit UserService(UserRepository& repo);
///   };
///
/// And register them with the server builder:
///
///   auto app = novaboot::Server::create()
///       .di_scan<UserService, UserRepository, PaymentService>()
///       .build();

#include "novaboot/di/scope.h"
#include "novaboot/di/attributes.h"
#include "novaboot/di/lifecycle.h"
#include "novaboot/di/inject.h"
#include "novaboot/di/container.h"
#include "novaboot/di/module.h"
#include "novaboot/di/handler_injector.h"
