// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(warnings)]
#![feature(futures_api, pin, arbitrary_self_types)]

extern crate clap;
extern crate failure;
extern crate fidl_fuchsia_overnet;
extern crate fuchsia_app as app;
extern crate fuchsia_async as async;
extern crate futures;

use clap::{App, SubCommand};
use failure::{Error, ResultExt};
use fidl_fuchsia_overnet::{OvernetMarker, OvernetProxy};
use futures::{future::lazy, prelude::*};
use async::temp::TempFutureExt;

fn app<'a, 'b>() -> App<'a, 'b> {
    App::new("onet")
        .version("0.1.0")
        .about("Overnet debug tool")
        .author("Fuchsia Team")
        .subcommands(vec![
            SubCommand::with_name("ls-peers").about("Lists known peer node ids"),
        ])
}

fn ls_peers(svc: OvernetProxy) -> impl Future<Output = Result<(), Error>> {
    svc.list_peers()
        .map(|peers| {
            for peer in peers {
                println!("PEER: {:?}", peer);
            }
            Ok(())
        })
}

fn dump_error() -> impl Future<Output = Result<(), Error>> {
    lazy(|_| {
        let _ = app().print_help();
        println!("");
        Ok(())
    })
}

fn main() -> Result<(), Error> {
    let args = app().get_matches();

    let mut executor = async::Executor::new().context("error creating event loop")?;
    let svc = app::client::connect_to_service::<OvernetMarker>()
        .context("Failed to connect to overnet service")?;

    let fut = match args.subcommand_name() {
        Some("ls-peers") => ls_peers(svc).left_future(),
        _ => dump_error().right_future(),
    };
    executor.run_singlethreaded(fut).map_err(Into::into)
}
