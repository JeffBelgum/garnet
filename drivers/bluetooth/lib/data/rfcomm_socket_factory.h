// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_BLUETOOTH_LIB_DATA_RFCOMM_SOCKET_FACTORY_H_
#define GARNET_DRIVERS_BLUETOOTH_LIB_DATA_RFCOMM_SOCKET_FACTORY_H_

#include "garnet/drivers/bluetooth/lib/common/byte_buffer.h"
#include "garnet/drivers/bluetooth/lib/data/socket_factory.h"
#include "garnet/drivers/bluetooth/lib/rfcomm/channel.h"
#include "garnet/drivers/bluetooth/lib/rfcomm/rfcomm.h"

namespace btlib {
namespace data {
namespace internal {

using RfcommSocketFactory =
    SocketFactory<rfcomm::Channel, rfcomm::DLCI, common::ByteBufferPtr>;

}  // namespace internal
}  // namespace data
}  // namespace btlib

#endif  // GARNET_DRIVERS_BLUETOOTH_LIB_DATA_RFCOMM_SOCKET_FACTORY_H_
