// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_VMM_INSTANCE_CONTROLLER_IMPL_H_
#define GARNET_BIN_GUEST_VMM_INSTANCE_CONTROLLER_IMPL_H_

#include <fuchsia/guest/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>

#include "garnet/lib/machina/input_dispatcher_impl.h"
#include "garnet/lib/machina/phys_mem.h"
#include "lib/component/cpp/startup_context.h"
#include "lib/fidl/cpp/binding_set.h"

// Provides an implementation of the |fuchsia::guest::InstanceController|
// interface. This exposes some guest services over FIDL.
class InstanceControllerImpl : public fuchsia::guest::InstanceController {
 public:
  InstanceControllerImpl(component::StartupContext* context,
                         const machina::PhysMem& phys_mem);

  void SetViewProvider(::fuchsia::ui::viewsv1::ViewProvider* view_provider) {
    view_provider_ = view_provider;
  }
  void SetInputDispatcher(machina::InputDispatcherImpl* input_dispatcher) {
    input_dispatcher_ = input_dispatcher;
  }

  // Extracts the socket handle to be used for the host end of serial
  // communication. The other end of this socket will be provided to clients
  // via |GetSerial|.
  zx::socket TakeSocket() { return std::move(server_socket_); }

  // |fuchsia::guest::InstanceController|
  void GetPhysicalMemory(GetPhysicalMemoryCallback callback) override;
  void GetSerial(GetSerialCallback callback) override;
  void GetViewProvider(GetViewProviderCallback callback) override;
  void GetInputDispatcher(
      fidl::InterfaceRequest<fuchsia::ui::input::InputDispatcher>
          input_dispatcher_request) override;

 private:
  fidl::BindingSet<fuchsia::guest::InstanceController> bindings_;
  fidl::BindingSet<::fuchsia::ui::viewsv1::ViewProvider>
      view_provider_bindings_;
  fidl::BindingSet<fuchsia::ui::input::InputDispatcher>
      input_dispatcher_bindings_;

  const zx::vmo vmo_;
  zx::socket server_socket_;
  zx::socket client_socket_;
  ::fuchsia::ui::viewsv1::ViewProvider* view_provider_;
  machina::InputDispatcherImpl* input_dispatcher_;
};

#endif  // GARNET_BIN_GUEST_VMM_INSTANCE_CONTROLLER_IMPL_H_
