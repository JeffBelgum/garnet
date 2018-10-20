// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "l2cap_socket_factory.h"

#include "socket_factory.cc"

namespace btlib {
namespace data {
namespace internal {

template class SocketFactory<l2cap::Channel, l2cap::ChannelId, l2cap::SDU>;

}  // namespace internal
}  // namespace data
}  // namespace btlib
