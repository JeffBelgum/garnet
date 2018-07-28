// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro, futures_api)]
#![deny(warnings)]

use fidl_fuchsia_wlan_device as wlan;
use fuchsia_async as fasync;
use fuchsia_wlan_dev as wlan_dev;

use failure::{format_err, Error, ResultExt};
use std::convert::Into;
use std::fs::{File, OpenOptions};
use std::path::Path;

mod sys;

const DEV_TEST: &str = "/dev/test/test";
const DEV_WLANPHY: &str = "/dev/test/test/wlan/wlanphy-test";
const WLAN: &str = "wlan";
const WLAN_DRIVER_NAME: &str = "/system/driver/wlanphy-testdev.so";

fn usage(appname: &str) {
    eprintln!("usage: {} [add|rm|query|create|destroy <n>]", appname);
}

fn open_rdwr<P: AsRef<Path>>(path: P) -> Result<File, Error> {
    OpenOptions::new().read(true).write(true).open(path).map_err(Into::into)
}

fn add_wlanphy() -> Result<(), Error> {
    let devpath = sys::create_test_device(DEV_TEST, WLAN)?;
    println!("created test device at {:?}", devpath);

    // The device created above might not show up in the /dev filesystem right away. Loop until we
    // have the device opened (or we give up).
    // Note: a directory watcher could work too, but may be a bit heavy for this use-case.
    let mut retry = 0;
    let mut dev = None;
    {
        while retry < 100 {
            retry += 1;
            if let Ok(d) = open_rdwr(&devpath) {
                dev = Some(d);
                break;
            }
        }
    }
    let dev = dev.ok_or(format_err!("could not open {:?}", devpath))?;

    sys::bind_test_device(&dev, WLAN_DRIVER_NAME)
}

fn rm_wlanphy() -> Result<(), Error> {
    let path = Path::new(DEV_TEST).join(WLAN);
    let dev = open_rdwr(path.clone())?;

    sys::destroy_test_device(&dev)
        .map(|_| println!("{:?} destroyed", path))
}

async fn query_wlanphy(proxy: wlan::PhyProxy) -> Result<(), Error> {
    let resp = await!(proxy.query())?;
    println!("query results: {:?}", resp.info);
    Ok(())
}

async fn create_wlanintf(proxy: wlan::PhyProxy) -> Result<(), Error> {
    let mut req = wlan::CreateIfaceRequest { role: wlan::MacRole::Client };
    let resp = await!(proxy.create_iface(&mut req))?;
    println!("create results: {:?}", resp);
    Ok(())
}

async fn destroy_wlanintf(proxy: wlan::PhyProxy, id: u16) -> Result<(), Error> {
    let mut req = wlan::DestroyIfaceRequest { id: id };
    let resp = await!(proxy.destroy_iface(&mut req))?;
    println!("destroyed intf {} resp: {:?}", id, resp);
    Ok(())
}

fn main() -> Result<(), Error> {
    let args: Vec<_> = std::env::args().collect();
    let appname = &args[0];
    if args.len() < 2 {
        usage(appname);
        return Ok(());
    }
    let command = &args[1];

    let mut exec = fasync::Executor::new().context("error creating event loop")?;
    let phy = wlan_dev::Device::new(DEV_WLANPHY)?;
    let proxy = wlan_dev::connect_wlan_phy(&phy)?;

    let fut = async {
        match command.as_ref() {
            "add" => add_wlanphy(),
            "rm" => rm_wlanphy(),
            "query" => await!(query_wlanphy(proxy)),
            "create" => await!(create_wlanintf(proxy)),
            "destroy" => {
                if args.len() < 3 {
                    usage(appname);
                    Ok(())
                } else {
                    let id = u16::from_str_radix(&args[2], 10)?;
                    await!(destroy_wlanintf(proxy, id))
                }
            },
            _ => {
                usage(appname);
                Ok(())
            }
        }
    };
    exec.run_singlethreaded(fut)
}


