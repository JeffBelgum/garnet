// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_HTTP_NET_ERRORS_H_
#define GARNET_BIN_HTTP_NET_ERRORS_H_

#include <string>

namespace network {

enum Error {
  // No error.
  OK = 0,

#define NET_ERROR(label, value) NETWORK_ERR_##label = value,
#include "garnet/bin/http/net_error_list.h"
#undef NET_ERROR
};

// Returns a textual representation of the error code for logging purposes.
std::string ErrorToString(int error);

// Same as above, but leaves off the leading "network::".
std::string ErrorToShortString(int error);

}  // namespace network

#endif  // GARNET_BIN_HTTP_NET_ERRORS_H_
