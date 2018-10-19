// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! ril-ctl is used for interacting with devices that expose a QMI RIL over
//! FIDL on Fuchsia.
//!
//! Ex: run ril-ctl -d /dev/class/ril-transport/000
//!
//! Future support for connecting through modem-mgr instead of owning the
//! modem service is planned. A REPL is also planned as the FIDL interfaces
//! evolve.

#![feature(async_await, await_macro, futures_api)]
//#![deny(warnings)]

use failure::{format_err, Error, ResultExt};
use fidl::endpoints::create_proxy;
use fidl_fuchsia_telephony_ril::RadioInterfaceLayerMarker;
use fuchsia_app::client::Launcher;
use fuchsia_async as fasync;
use qmi;
use std::env;
use std::fs::File;

pub fn main() -> Result<(), Error> {
    let mut exec = fasync::Executor::new().context("error creating event loop")?;
//    let (_client_proxy, client_server) = create_proxy()?;

    let args: Vec<String> = env::args().collect();

    // TODO more advanced arg parsing
    // TODO chose what service we are launching from CLI (ex: ril-at)
    if args.len() != 3 || args[1] != "-d" {
        eprintln!("ril-ctl -d <ril-transport-device path>");
        ::std::process::exit(1);
    }

    let launcher = Launcher::new().context("Failed to open launcher service")?;
    let app = launcher
        .launch(String::from("ril-qmi"), None)
        .context("Failed to launch ril-qmi service")?;
    let ril_modem = app.connect_to_service(RadioInterfaceLayerMarker)?;

    let path = &args[2];

    let file = File::open(&path)?;
    let chan = qmi::connect_transport_device(&file)?;

    let client_fut = async {
        let connected_transport = await!(ril_modem.connect_transport(chan))?;
        if connected_transport {
            let client_res = await!(ril_modem.get_device_identity())?;
            eprintln!("resp: {:?}", client_res);
            return Ok(());
        }
        Err(format_err!("Failed to request modem or client"))
    };
    exec.run_singlethreaded(client_fut)
}