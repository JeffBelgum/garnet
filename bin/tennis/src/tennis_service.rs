// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::game::{Game, PlayerState};
use failure::ResultExt;
use fidl::endpoints::{ClientEnd, RequestStream, ServerEnd};
use fidl_fuchsia_game_tennis as fidl_tennis;
use fuchsia_async as fasync;
use fuchsia_syslog::{fx_log, fx_log_err, fx_log_info};
use fuchsia_zircon::DurationNum;
use futures::prelude::*;
use parking_lot::Mutex;
use std::sync::{Arc, Weak};

#[derive(Clone)]
pub struct TennisService(Arc<Mutex<Game>>);

impl TennisService {
    pub fn new() -> TennisService {
        TennisService(Arc::new(Mutex::new(Game::new())))
    }

    pub fn bind(&self, chan: fuchsia_async::Channel) {
        let mut self_clone = self.clone();
        let mut stream = fidl_tennis::TennisServiceRequestStream::from_channel(chan);
        fuchsia_async::spawn(
            async move {
                while let Some(msg) = await!(stream.try_next())
                    .context("error reading value from tennis service request stream")?
                {
                    match msg {
                        fidl_tennis::TennisServiceRequest::GetState { responder, .. } => {
                            let TennisService(game_arc) = &self_clone;
                            responder.send(&mut game_arc.lock().state());
                        }
                        fidl_tennis::TennisServiceRequest::RegisterPaddle {
                            player_name,
                            paddle,
                            ..
                        } => {
                            fx_log_info!("new paddle registered: {}", player_name);
                            self_clone.register_paddle(player_name, paddle);
                        }
                    }
                }
                Ok(())
            }
                .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
        );
    }

    pub fn register_paddle(
        &self, player_name: String,
        paddle: fidl::endpoints::ClientEnd<fidl_fuchsia_game_tennis::PaddleMarker>,
    ) {
        let TennisService(game_arc) = self.clone();
        let mut game = game_arc.lock();
        let player_state = game.register_new_paddle(player_name);
        let mut stream = paddle.into_proxy().unwrap().take_event_stream(); // TODO(lard): remove unwrap
        fasync::spawn(
            async move {
                while let Some(event) = await!(stream.try_next())
                    .context("error reading value from paddle event stream")?
                {
                    let state = match event {
                        fidl_tennis::PaddleEvent::Up { .. } => PlayerState::Up,
                        fidl_tennis::PaddleEvent::Down { .. } => PlayerState::Down,
                        fidl_tennis::PaddleEvent::Stop { .. } => PlayerState::Stop,
                    };
                    *player_state.lock() = state;
                }
                Ok(())
            }
                .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
        );
        if game.players_ready() {
            fx_log_info!("game is beginning");
            let game_arc = game_arc.clone();
            fasync::spawn(
                async move {
                    loop {
                        game_arc.lock().step();
                        let time_step: i64 = 1000 / 5;
                        await!(fuchsia_async::Timer::new(time_step.millis().after_now()));
                    }
                },
            );
        }
    }
}
