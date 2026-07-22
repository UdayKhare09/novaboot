#pragma once

#include <functional>

#include "novaboot/actuator/actuator.h"
#include "novaboot/db/db_client.h"

namespace novaboot::db {

/// Creates a lightweight synchronous Actuator health contributor for a data
/// source. It validates acquisition and a portable `SELECT 1` query; callers
/// should use a datasource whose acquisition timeout is already bounded.
inline std::function<actuator::Health()> health_contributor(DataSource& source) {
    return [&source] {
        try {
            auto connection = source.get_connection();
            auto result = connection->query("SELECT 1");
            if (!result || !result->next()) {
                return actuator::Health{actuator::HealthStatus::Down,
                                        {{"error", "health query returned no rows"}}};
            }
            return actuator::Health{actuator::HealthStatus::Up, {}};
        } catch (const std::exception& error) {
            return actuator::Health{actuator::HealthStatus::Down,
                                    {{"error", error.what()}}};
        } catch (...) {
            return actuator::Health{actuator::HealthStatus::Down,
                                    {{"error", "unknown database health failure"}}};
        }
    };
}

} // namespace novaboot::db
