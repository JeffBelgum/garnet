// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(warnings)]

#![feature(try_from,async_await,await_macro)]

use failure::{Error, ResultExt};
use fuchsia_async as fasync;
use fidl::endpoints::create_endpoints;
use fuchsia_app::client::connect_to_service;
use fidl_fuchsia_game_tennis::{TennisServiceMarker, PaddleRequest};
use fuchsia_zircon::DurationNum;
use parking_lot::Mutex;
use std::sync::Arc;
use futures::TryStreamExt;

fn main() -> Result<(), Error> {
    let mut executor = fasync::Executor::new().context("Error creating executor")?;
    let tennis_service = connect_to_service::<TennisServiceMarker>()?;

    let (client_end, paddle_controller) = create_endpoints()?;

    let (mut prs, paddle_control_handle) = paddle_controller.into_stream_and_control_handle()?;

    tennis_service.register_paddle("example_ai", client_end)?;

    let i_am_player_2 = Arc::new(Mutex::new(false));
    let i_am_player_2_clone = i_am_player_2.clone();

    println!("registering with game service");
    fasync::spawn(
        async move {
            while let Some(PaddleRequest::NewGame{is_player_2, ..}) = await!(prs.try_next()).unwrap() { // TODO: remove unwrap
                if is_player_2 {
                    println!("I am player 2");
                } else {
                    println!("I am player 1");
                }
                *i_am_player_2_clone.lock() = is_player_2
            }
        }
    );

    let resp: Result<(), Error> = executor.run_singlethreaded(async move {
        loop {
            let time_step: i64 = 1000 / 5;
            await!(fuchsia_async::Timer::new(time_step.millis().after_now()));

            let state = await!(tennis_service.get_state())?;
            if state.game_num == 0 {
                continue;
            }
            let my_y;
            if *i_am_player_2.lock() {
                my_y = state.player_2_y;
            } else {
                my_y = state.player_1_y;
            }
            if state.ball_y > my_y {
                println!("moving down");
                paddle_control_handle.send_down()?;
            } else if state.ball_y < my_y{
                println!("moving up");
                paddle_control_handle.send_up()?;
            } else {
                println!("staying still");
                paddle_control_handle.send_stop()?;
            }
        }
    });
    resp
}
