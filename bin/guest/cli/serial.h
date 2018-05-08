// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_CLI_SERIAL_H_
#define GARNET_BIN_GUEST_CLI_SERIAL_H_

#include "garnet/bin/guest/cli/service.h"

void handle_serial(uint32_t guest_id);
void handle_serial(guest::GuestController* guest_controller);

#endif  // GARNET_BIN_GUEST_CLI_SERIAL_H_
