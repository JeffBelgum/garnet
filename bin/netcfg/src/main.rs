// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(warnings)]

extern crate failure;
#[macro_use]
extern crate serde_derive;
extern crate serde_json;
extern crate fidl;
extern crate fidl_fuchsia_devicesettings;
extern crate fidl_fuchsia_netstack as netstack;
extern crate fuchsia_app as app;
extern crate fuchsia_async as async;
extern crate fuchsia_zircon as zx;
extern crate futures;

use async::temp::TempFutureExt;
use failure::{Error, ResultExt};
use fidl_fuchsia_devicesettings::{DeviceSettingsManagerMarker};
use netstack::{NetstackMarker, NetInterface, NetstackEvent, INTERFACE_FEATURE_SYNTH, INTERFACE_FEATURE_LOOPBACK};
use std::fs;
use futures::{future, StreamExt, TryFutureExt, TryStreamExt};

mod device_id;

const DEFAULT_CONFIG_FILE: &str = "/pkg/data/default.json";

#[derive(Debug, Deserialize)]
pub struct Config {
    pub device_name: Option<String>,
}

fn is_physical(n: &NetInterface) -> bool {
    (n.features & (INTERFACE_FEATURE_SYNTH | INTERFACE_FEATURE_LOOPBACK)) == 0
}

fn derive_device_name(interfaces: Vec<NetInterface>) -> Option<String> {
    interfaces.iter()
        .filter(|iface| is_physical(iface))
        .min_by(|a, b| a.id.cmp(&b.id))
        .map(|iface| device_id::device_id(&iface.hwaddr))
}

static DEVICE_NAME_KEY: &str = "DeviceName";

fn main() -> Result<(), Error> {
    println!("netcfg: started");
    let default_config_file = fs::File::open(DEFAULT_CONFIG_FILE)?;
    let default_config: Config = serde_json::from_reader(default_config_file)?;
    let mut executor = async::Executor::new().context("error creating event loop")?;
    let netstack = app::client::connect_to_service::<NetstackMarker>().context("failed to connect to netstack")?;
    let device_settings_manager = app::client::connect_to_service::<DeviceSettingsManagerMarker>()
        .context("failed to connect to device settings manager")?;

    let device_name = match default_config.device_name {
        Some(name) => {
            device_settings_manager.set_string(DEVICE_NAME_KEY, &name).map_ok(|_| ()).left_future()
        },
        None => {
            netstack.take_event_stream().try_filter_map(|NetstackEvent::OnInterfacesChanged { interfaces: is }| {
                future::ready(Ok(derive_device_name(is)))
            }).take(1).try_for_each(|name| {
                device_settings_manager.set_string(DEVICE_NAME_KEY, &name).map_ok(|_| ())
            }).map_ok(|_| ()).right_future()
        },
    }.map_err(Into::into);

    executor.run_singlethreaded(device_name)
}
