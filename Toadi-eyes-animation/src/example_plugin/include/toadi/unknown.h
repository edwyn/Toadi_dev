// Reconstructed from libBroker.so / libPlugins.so (Toadi app 6.8.0-202607011333+3b3ec6e)
//
// toadi::Unknown is the root of every object stored in the Broker registry.
// It is NOT empty: it has two virtual methods after the destructor.
//
// vtable layout (aarch64, Itanium C++ ABI):
//   +0x00  ~Unknown()            (complete object destructor, D1)
//   +0x08  ~Unknown()            (deleting destructor, D0)
//   +0x10  bool QueryInterface(Unknown&)
//   +0x18  std::vector<std::string> GetInterfaces() const
//
// Evidence: libBroker.so BrokerImpl::Register calls vtable+0x18 and iterates
// the returned vector of names; InterfaceImpl<T,...>::QueryInterface calls
// vtable+0x18 on the query object and dispatches through SQI<T>.
#pragma once

#include <string>
#include <vector>

namespace toadi {

class Unknown {
public:
    virtual ~Unknown() = default;

    // Typed query negotiation. A registered object receives the caller's
    // QueryInterface<T> object here; returning false means "not me".
    // A minimal plug-in may always return false.
    virtual bool QueryInterface(Unknown& query) = 0;

    // Names under which this object is registered in the Broker. The Broker
    // rejects registration if the list contains duplicates or if any name is
    // already registered.
    virtual std::vector<std::string> GetInterfaces() const = 0;
};

} // namespace toadi
