// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crashpad_analyzer_impl.h"

#include <utility>

#include <fuchsia/crash/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/syslog/cpp/logger.h>

int main(int argc, const char** argv) {
  syslog::InitLogger({"crash"});

  std::unique_ptr<fuchsia::crash::CrashpadAnalyzerImpl> analyzer =
      fuchsia::crash::CrashpadAnalyzerImpl::TryCreate();
  if (!analyzer) {
    return EXIT_FAILURE;
  }

  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  std::unique_ptr<component::StartupContext> app_context(
      component::StartupContext::CreateFromStartupInfo());
  fidl::BindingSet<::fuchsia::crash::Analyzer> bindings;
  app_context->outgoing().AddPublicService(bindings.GetHandler(analyzer.get()));

  loop.Run();

  return EXIT_SUCCESS;
}
