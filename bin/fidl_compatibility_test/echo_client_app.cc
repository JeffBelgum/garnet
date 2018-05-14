// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/compatibility_test_service.h>
#include "garnet/bin/fidl_compatibility_test/echo_client_app.h"

namespace compatibility_test_service {

EchoClientApp::EchoClientApp()
    : context_(component::ApplicationContext::CreateFromStartupInfo()) {}

EchoPtr& EchoClientApp::echo() { return echo_; }

void EchoClientApp::Start(std::string server_url) {
  component::ApplicationLaunchInfo launch_info;
  launch_info.url = server_url;
  launch_info.directory_request = echo_provider_.NewRequest();
  context_->launcher()->CreateApplication(std::move(launch_info),
                                          controller_.NewRequest());

  echo_provider_.ConnectToService(echo_.NewRequest().TakeChannel(),
                                  Echo::Name_);
}
}  // namespace compatibility_test_service
