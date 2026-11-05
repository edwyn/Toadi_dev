// Reconstructed from libBroker.so (Toadi app 6.8.0-202607011333+3b3ec6e)
//
// toadi::Broker is the in-process, string-keyed service registry. It is a pure
// interface with three virtual methods after the destructor.
//
// vtable layout (aarch64, Itanium C++ ABI):
//   +0x00  ~Broker()                                  (D1)
//   +0x08  ~Broker()                                  (D0)
//   +0x10  bool registerInterface(std::unique_ptr<Unknown>)
//   +0x18  void unregisterInterface(const std::string&)
//   +0x20  QueryInterface<Unknown> queryInterface(const std::string&) const
//
// makeBroker() builds a BrokerImpl (0x70 bytes: vptr, pthread_rwlock_t at +0x08,
// std::map<std::string, std::shared_ptr<Unknown>> at +0x40) and returns it as a
// unique_ptr. makeBroker(Broker&) builds a LocalBrokerImpl (0x18 bytes: vptr,
// Broker* parent, BrokerImpl* local cache) which delegates misses to the parent.
#pragma once

#include <memory>
#include <string>

#include "toadi/query_interface.h"
#include "toadi/unknown.h"

namespace toadi {

class Broker {
public:
    virtual ~Broker() = default;

    // Takes ownership. Returns false if any name in GetInterfaces() is a
    // duplicate or is already registered.
    virtual bool registerInterface(std::unique_ptr<Unknown> iface) = 0;

    // Removes the entry registered under this name. No-op if absent.
    virtual void unregisterInterface(const std::string& name) = 0;

    // Looks up a name. The returned value holds the registered object
    // (get() is null when the name is unknown) and the requested name.
    virtual QueryInterface<Unknown> queryInterface(const std::string& name) const = 0;
};

// Global broker (libBroker.so: toadi::makeBroker()).
std::unique_ptr<Broker> makeBroker();

// Local broker that forwards unresolved queries to the supplied parent
// (libBroker.so: toadi::makeBroker(toadi::Broker&)).
std::unique_ptr<Broker> makeBroker(Broker& parent);

} // namespace toadi
