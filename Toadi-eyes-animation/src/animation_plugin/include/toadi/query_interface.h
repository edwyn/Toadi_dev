// Reconstructed from libBroker.so (Toadi app 6.8.0-202607011333+3b3ec6e).
//
// Broker::queryInterface() returns a QueryInterface<Unknown> VALUE, not a
// shared_ptr<Unknown>. Evidence (BrokerImpl::queryInterface, 0x143d8):
//
//   miss:  stp xzr, xzr, [sp, #8]        // shared_ptr = null
//          x1 = sp+8; x0 = x8 (sret); x2 = name
//          bl QueryInterface<Unknown>::QueryInterface(shared_ptr, string)
//   hit:   loads the registered shared_ptr, then the same construction.
//
// The object layout (QueryInterface<Unknown> constructor, 0x146a8) is:
//   +0x00  std::shared_ptr<Unknown>   (object, control_block)
//   +0x10  std::string                (requested interface name)
//   size 0x30
//
// Typed consumers wrap this in QueryInterface<T> and run the SQI<T>
// negotiation; that template machinery lives in the consumer libraries.
// For a plug-in, reading get()/valid() is enough.
#pragma once

#include <memory>
#include <string>
#include <utility>

#include "toadi/unknown.h"

namespace toadi {

template <typename T>
class QueryInterface;

// Specialisation returned by Broker::queryInterface(). It is deliberately
// layout-identical to the shipped class: shared_ptr at +0, std::string at
// +0x10, no vtable.
template <>
class QueryInterface<Unknown> {
public:
    QueryInterface() = default;

    QueryInterface(std::shared_ptr<Unknown> object, std::string name)
        : object_(std::move(object)), name_(std::move(name)) {}

    ~QueryInterface() = default;

    QueryInterface(const QueryInterface&) = default;
    QueryInterface(QueryInterface&&) noexcept = default;
    QueryInterface& operator=(const QueryInterface&) = default;
    QueryInterface& operator=(QueryInterface&&) noexcept = default;

    // The registered object, or null when the name is unknown.
    const std::shared_ptr<Unknown>& get() const { return object_; }
    bool valid() const { return static_cast<bool>(object_); }
    const std::string& name() const { return name_; }

private:
    std::shared_ptr<Unknown> object_;
    std::string name_;
};

}  // namespace toadi
