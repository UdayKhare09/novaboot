#pragma once

#include <stdexcept>
#include <string>

namespace novaboot::data {

struct DataException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct EntityNotFoundException : public DataException {
    using DataException::DataException;
};

struct CacheUnavailableException : public DataException {
    using DataException::DataException;
};

struct DataSourceException : public DataException {
    using DataException::DataException;
};

struct OptimisticLockException : public DataException {
    using DataException::DataException;
};

} // namespace novaboot::data
