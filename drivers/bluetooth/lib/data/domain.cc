// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "domain.h"

#include "garnet/drivers/bluetooth/lib/common/log.h"
#include "garnet/drivers/bluetooth/lib/common/task_domain.h"
#include "garnet/drivers/bluetooth/lib/data/l2cap_socket_factory.h"
#include "garnet/drivers/bluetooth/lib/hci/transport.h"
#include "garnet/drivers/bluetooth/lib/l2cap/channel_manager.h"
#include "garnet/drivers/bluetooth/lib/rfcomm/channel_manager.h"

namespace btlib {
namespace data {

class Impl final : public Domain, public common::TaskDomain<Impl, Domain> {
 public:
  Impl(fxl::RefPtr<hci::Transport> hci, std::string thread_name)
      : Domain(),
        common::TaskDomain<Impl, Domain>(this, std::move(thread_name)),
        hci_(hci) {
    ZX_DEBUG_ASSERT(hci_);
  }

  void Initialize() override {
    PostMessage([this] {
      // This can only run once during initialization.
      ZX_DEBUG_ASSERT(!l2cap_);
      ZX_DEBUG_ASSERT(!rfcomm_);

      InitializeL2CAP();
      InitializeRFCOMM();
      socket_factory_ = std::make_unique<internal::L2capSocketFactory>();

      bt_log(TRACE, "data-domain", "initialized");
    });
  }

  void ShutDown() override {
    common::TaskDomain<Impl, Domain>::ScheduleCleanUp();
  }

  // Called by the domain dispatcher as a result of ScheduleCleanUp().
  void CleanUp() {
    AssertOnDispatcherThread();
    bt_log(TRACE, "data-domain", "shutting down");
    rfcomm_ = nullptr;
    l2cap_ = nullptr;  // Unregisters the RFCOMM PSM.
  }

  void AddACLConnection(hci::ConnectionHandle handle,
                        hci::Connection::Role role,
                        l2cap::LinkErrorCallback link_error_callback,
                        async_dispatcher_t* dispatcher) override {
    PostMessage([this, handle, role, lec = std::move(link_error_callback),
                 dispatcher]() mutable {
      if (l2cap_) {
        l2cap_->RegisterACL(handle, role, std::move(lec), dispatcher);
      }
    });
  }

  void AddLEConnection(
      hci::ConnectionHandle handle, hci::Connection::Role role,
      l2cap::LinkErrorCallback link_error_callback,
      l2cap::LEFixedChannelsCallback channel_callback,
      l2cap::LEConnectionParameterUpdateCallback conn_param_callback,
      async_dispatcher_t* dispatcher) override {
    PostMessage([this, handle, role, cp_cb = std::move(conn_param_callback),
                 link_err_cb = std::move(link_error_callback),
                 chan_cb = std::move(channel_callback), dispatcher]() mutable {
      if (l2cap_) {
        l2cap_->RegisterLE(handle, role, std::move(cp_cb),
                           std::move(link_err_cb), dispatcher);

        auto att = l2cap_->OpenFixedChannel(handle, l2cap::kATTChannelId);
        auto smp = l2cap_->OpenFixedChannel(handle, l2cap::kLESMPChannelId);
        ZX_DEBUG_ASSERT(att);
        ZX_DEBUG_ASSERT(smp);
        async::PostTask(dispatcher, [att = std::move(att), smp = std::move(smp),
                                     cb = std::move(chan_cb)]() mutable {
          cb(std::move(att), std::move(smp));
        });
      }
    });
  }

  void RemoveConnection(hci::ConnectionHandle handle) override {
    PostMessage([this, handle] {
      if (l2cap_) {
        l2cap_->Unregister(handle);
      }
    });
  }

  void OpenL2capChannel(hci::ConnectionHandle handle, l2cap::PSM psm,
                        l2cap::ChannelCallback cb,
                        async_dispatcher_t* dispatcher) override {
    ZX_DEBUG_ASSERT(dispatcher);
    PostMessage([this, handle, psm, cb = std::move(cb), dispatcher]() mutable {
      if (l2cap_) {
        l2cap_->OpenChannel(handle, psm, std::move(cb), dispatcher);
      }
    });
  }

  void RegisterService(l2cap::PSM psm, l2cap::ChannelCallback callback,
                       async_dispatcher_t* dispatcher) override {
    PostMessage([this, psm, callback = std::move(callback),
                 dispatcher]() mutable {
      if (l2cap_) {
        const bool result =
            l2cap_->RegisterService(psm, std::move(callback), dispatcher);
        ZX_DEBUG_ASSERT(result);
      } else {
        // RegisterService could be called early in host initialization, so log
        // cases where L2CAP isn't ready for a service handler.
        bt_log(WARN, "l2cap",
               "failed to register handler for PSM %#.4x while uninitialized",
               psm);
      }
    });
  }

  void RegisterService(l2cap::PSM psm, SocketCallback socket_callback,
                       async_dispatcher_t* cb_dispatcher) override {
    RegisterService(
        psm,
        [this, psm, cb = std::move(socket_callback),
         cb_dispatcher](auto channel) mutable {
          zx::socket s = socket_factory_->MakeSocketForChannel(channel);
          // Called every time the service is connected, cb must be shared.
          async::PostTask(cb_dispatcher,
                          [s = std::move(s), cb = cb.share(),
                           handle = channel->link_handle()]() mutable {
                            cb(std::move(s), handle);
                          });
        },
        dispatcher());
  }

  void UnregisterService(l2cap::PSM psm) override {
    PostMessage([this, psm] {
      if (l2cap_) {
        l2cap_->UnregisterService(psm);
      }
    });
  }

 private:
  void InitializeL2CAP() {
    AssertOnDispatcherThread();
    l2cap_ = std::make_unique<l2cap::ChannelManager>(hci_, dispatcher());
  }

  void InitializeRFCOMM() {
    AssertOnDispatcherThread();

    // |this| is safe to capture here since |rfcomm_| is owned by |this| and the
    // delegate callback only runs synchronously on |rfcomm_|'s creation thread
    // as a result of OpenRemoteChannel.
    rfcomm_ = std::make_unique<rfcomm::ChannelManager>(
        [this](hci::ConnectionHandle handle, l2cap::ChannelCallback cb) {
          l2cap_->OpenChannel(handle, l2cap::kRFCOMM, std::move(cb),
                              dispatcher());
        });

    // Claim the RFCOMM PSM for inbound connections.
    auto rfcomm_cb = [self = fbl::WrapRefPtr(this)](auto channel) {
      ZX_DEBUG_ASSERT(channel);
      if (!self->rfcomm_) {
        bt_log(SPEW, "data-domain", "RFCOMM connected after shutdown");
        return;
      }
      if (self->rfcomm_->RegisterL2CAPChannel(std::move(channel))) {
        bt_log(TRACE, "data-domain", "RFCOMM session initialized");
      } else {
        bt_log(ERROR, "data-domain",
               "failed to initialize RFCOMM session after L2CAP connection");
      }
    };

    // Registering the RFCOMM PSM immediately after creation should always
    // succeed.
    bool result = l2cap_->RegisterService(l2cap::kRFCOMM, std::move(rfcomm_cb),
                                          dispatcher());
    ZX_ASSERT_MSG(result, "failed to register RFCOMM PSM");
  }

  // All members below must be accessed on the data domain thread.

  // Handle to the underlying HCI transport.
  fxl::RefPtr<hci::Transport> hci_;

  std::unique_ptr<l2cap::ChannelManager> l2cap_;
  std::unique_ptr<rfcomm::ChannelManager> rfcomm_;

  // Creates sockets that bridge internal L2CAP and RFCOMM channels to profile
  // processes.
  std::unique_ptr<internal::L2capSocketFactory> socket_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Impl);
};

// static
fbl::RefPtr<Domain> Domain::Create(fxl::RefPtr<hci::Transport> hci,
                                   std::string thread_name) {
  ZX_DEBUG_ASSERT(hci);
  return AdoptRef(new Impl(hci, std::move(thread_name)));
}

}  // namespace data
}  // namespace btlib
