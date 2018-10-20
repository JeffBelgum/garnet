// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_BLUETOOTH_LIB_DATA_L2CAP_SOCKET_FACTORY_H_
#define GARNET_DRIVERS_BLUETOOTH_LIB_DATA_L2CAP_SOCKET_FACTORY_H_

#include "garnet/drivers/bluetooth/lib/data/socket_factory.h"
#include "garnet/drivers/bluetooth/lib/l2cap/channel.h"
#include "garnet/drivers/bluetooth/lib/l2cap/l2cap.h"
#include "garnet/drivers/bluetooth/lib/l2cap/sdu.h"

namespace btlib {
namespace data {
namespace internal {

using L2capSocketFactory =
    SocketFactory<l2cap::Channel, l2cap::ChannelId, l2cap::SDU>;

}  // namespace internal
}  // namespace data
}  // namespace btlib

#endif  // GARNET_DRIVERS_BLUETOOTH_LIB_DATA_L2CAP_SOCKET_FACTORY_H_
