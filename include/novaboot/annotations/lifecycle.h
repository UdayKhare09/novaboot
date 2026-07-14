#pragma once

namespace novaboot::annotations {

/// Mark a component method to be invoked after construction and DI injection are complete.
struct PostConstruct {};

/// Mark a component method to be invoked before the DI container shuts down and destroys the bean.
struct PreDestroy {};

} // namespace novaboot::annotations
