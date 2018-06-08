// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FIDL_ENABLE_LEGACY_WAIT_FOR_RESPONSE

#include <fidl/examples/echo/cpp/fidl.h>
#include <launchpad/launchpad.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/default.h>
#include <lib/zx/process.h>
#include <zircon/processargs.h>

#include "lib/app/cpp/startup_context.h"
#include "lib/svc/cpp/services.h"

namespace echo2 {

class EchoClientApp {
 public:
  EchoClientApp()
      : context_(fuchsia::sys::StartupContext::CreateFromStartupInfo()) {}

  fidl::examples::echo::EchoPtr& echo() { return echo_; }

  void Start(std::string server_url) {
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = server_url;
    launch_info.directory_request = echo_provider_.NewRequest();
    context_->launcher()->CreateComponent(std::move(launch_info),
                                          controller_.NewRequest());

    echo_provider_.ConnectToService(echo_.NewRequest().TakeChannel(),
                                    fidl::examples::echo::Echo::Name_);
  }

 private:
  EchoClientApp(const EchoClientApp&) = delete;
  EchoClientApp& operator=(const EchoClientApp&) = delete;

  std::unique_ptr<fuchsia::sys::StartupContext> context_;
  fuchsia::sys::Services echo_provider_;
  fuchsia::sys::ComponentControllerPtr controller_;
  fidl::examples::echo::EchoPtr echo_;
};

}  // namespace echo2

int main(int argc, const char** argv) {
  std::string server_url = "echo2_server_cpp";
  std::string msg = "hello world";

  for (int i = 1; i < argc - 1; ++i) {
    if (!strcmp("--server", argv[i])) {
      server_url = argv[++i];
    } else if (!strcmp("-m", argv[i])) {
      msg = argv[++i];
    }
  }

  async::Loop loop(&kAsyncLoopConfigMakeDefault);

  echo2::EchoClientApp app;
  app.Start(server_url);

  app.echo()->EchoString(msg, [](fidl::StringPtr value) {
    printf("***** Response: %s\n", value->data());
  });

  return app.echo().WaitForResponse();
}
