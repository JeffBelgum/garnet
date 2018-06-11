// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/appmgr/namespace.h"

#include <fuchsia/process/cpp/fidl.h>
#include <lib/async/default.h>
#include <lib/fdio/util.h>

#include <utility>

#include "garnet/bin/appmgr/realm.h"
#include "lib/app/cpp/environment_services.h"

namespace fuchsia {
namespace sys {

Namespace::Namespace(fxl::RefPtr<Namespace> parent, Realm* realm,
                     ServiceListPtr service_list)
    : vfs_(async_get_default()),
      services_(fbl::AdoptRef(new ServiceProviderDirImpl())),
      parent_(parent),
      realm_(realm) {
  if (parent_) {
    services_->set_parent(parent_->services());
  }

  services_->AddService(
      fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
        environment_bindings_.AddBinding(
            this, fidl::InterfaceRequest<Environment>(std::move(channel)));
        return ZX_OK;
      })),
      Environment::Name_);
  services_->AddService(
      fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
        launcher_bindings_.AddBinding(
            this, fidl::InterfaceRequest<Launcher>(std::move(channel)));
        return ZX_OK;
      })),
      Launcher::Name_);
  services_->AddService(
      fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
        ConnectToEnvironmentService(
            fidl::InterfaceRequest<fuchsia::process::Launcher>(
                std::move(channel)));
        return ZX_OK;
      })),
      fuchsia::process::Launcher::Name_);

  if (service_list) {
    auto& names = service_list->names;
    additional_services_ = service_list->provider.Bind();
    for (auto& name : *names) {
      services_->AddService(
          fbl::AdoptRef(new fs::Service([this, name](zx::channel channel) {
            additional_services_->ConnectToService(name, std::move(channel));
            return ZX_OK;
          })),
          name);
    }
  }
}

Namespace::~Namespace() {}

void Namespace::AddBinding(fidl::InterfaceRequest<Environment> environment) {
  environment_bindings_.AddBinding(this, std::move(environment));
}

void Namespace::CreateNestedEnvironment(
    zx::channel host_directory, fidl::InterfaceRequest<Environment> environment,
    fidl::InterfaceRequest<EnvironmentController> controller,
    fidl::StringPtr label) {
  realm_->CreateNestedJob(std::move(host_directory), std::move(environment),
                          std::move(controller), label);
}

void Namespace::GetLauncher(fidl::InterfaceRequest<Launcher> launcher) {
  launcher_bindings_.AddBinding(this, std::move(launcher));
}

void Namespace::GetServices(fidl::InterfaceRequest<ServiceProvider> services) {
  services_->AddBinding(std::move(services));
}

zx_status_t Namespace::ServeServiceDirectory(zx::channel directory_request) {
  return vfs_.ServeDirectory(services_, std::move(directory_request));
}

void Namespace::CreateComponent(
    LaunchInfo launch_info,
    fidl::InterfaceRequest<ComponentController> controller) {
  realm_->CreateComponent(std::move(launch_info), std::move(controller));
}

zx::channel Namespace::OpenServicesAsDirectory() {
  zx::channel h1, h2;
  if (zx::channel::create(0, &h1, &h2) != ZX_OK)
    return zx::channel();
  if (ServeServiceDirectory(std::move(h1)) != ZX_OK)
    return zx::channel();
  return h2;
}

}  // namespace sys
}  // namespace fuchsia
