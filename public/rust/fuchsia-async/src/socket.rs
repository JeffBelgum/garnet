// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon::{self as zx, AsHandleRef};
use futures::io::{self, AsyncRead, AsyncWrite, Initializer};
use futures::{future::poll_fn, stream::Stream, task::LocalWaker, try_ready, Poll};
use std::fmt;
use std::pin::Pin;

use crate::RWHandle;

/// An I/O object representing a `Socket`.
pub struct Socket(RWHandle<zx::Socket>);

impl AsRef<zx::Socket> for Socket {
    fn as_ref(&self) -> &zx::Socket {
        self.0.get_ref()
    }
}

impl AsHandleRef for Socket {
    fn as_handle_ref(&self) -> zx::HandleRef {
        self.0.get_ref().as_handle_ref()
    }
}

impl From<Socket> for zx::Socket {
    fn from(socket: Socket) -> zx::Socket {
        socket.0.into_inner()
    }
}

impl Socket {
    /// Creates a new `Socket` from a previously-created `zx::Socket`.
    pub fn from_socket(socket: zx::Socket) -> Result<Socket, zx::Status> {
        Ok(Socket(RWHandle::new(socket)?))
    }

    /// Test whether this socket is ready to be read or not.
    ///
    /// If the socket is *not* readable then the current task is scheduled to
    /// get a notification when the socket does become readable. That is, this
    /// is only suitable for calling in a `Future::poll` method and will
    /// automatically handle ensuring a retry once the socket is readable again.
    fn poll_read(&self, lw: &LocalWaker) -> Poll<Result<(), zx::Status>> {
        self.0.poll_read(lw)
    }

    /// Test whether this socket is ready to be written to or not.
    ///
    /// If the socket is *not* writable then the current task is scheduled to
    /// get a notification when the socket does become writable. That is, this
    /// is only suitable for calling in a `Future::poll` method and will
    /// automatically handle ensuring a retry once the socket is writable again.
    fn poll_write(&self, lw: &LocalWaker) -> Poll<Result<(), zx::Status>> {
        self.0.poll_write(lw)
    }

    // Private helper for reading without `&mut` self.
    // This is used in the impls of `Read` for `Socket` and `&Socket`.
    fn read_nomut(&self, buf: &mut [u8], lw: &LocalWaker) -> Poll<Result<usize, zx::Status>> {
        try_ready!(self.poll_read(lw));
        let res = self.0.get_ref().read(buf);
        if res == Err(zx::Status::SHOULD_WAIT) {
            self.0.need_read(lw)?;
            return Poll::Pending;
        }
        if res == Err(zx::Status::PEER_CLOSED) {
            return Poll::Ready(Ok(0));
        }
        Poll::Ready(res)
    }

    // Private helper for writing without `&mut` self.
    // This is used in the impls of `Write` for `Socket` and `&Socket`.
    fn write_nomut(&self, buf: &[u8], lw: &LocalWaker) -> Poll<Result<usize, zx::Status>> {
        try_ready!(self.poll_write(lw));
        let res = self.0.get_ref().write(buf);
        if res == Err(zx::Status::SHOULD_WAIT) {
            self.0.need_write(lw)?;
            Poll::Pending
        } else {
            Poll::Ready(res)
        }
    }

    /// Polls for the next data on the socket, appending it to the end of |out| if it has arrived.
    /// Not very useful for a non-datagram socket as it will return all available data
    /// on the socket.
    pub fn poll_datagram(
        &self, out: &mut Vec<u8>, lw: &LocalWaker,
    ) -> Poll<Result<usize, zx::Status>> {
        try_ready!(self.0.poll_read(lw));
        let avail = self.0.get_ref().outstanding_read_bytes()?;
        let len = out.len();
        out.resize(len + avail, 0);
        let (_, mut tail) = out.split_at_mut(len);
        match self.0.get_ref().read(&mut tail) {
            Err(zx::Status::SHOULD_WAIT) => {
                self.0.need_read(lw)?;
                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
            Ok(bytes) => {
                if bytes == avail {
                    Poll::Ready(Ok(bytes))
                } else {
                    Poll::Ready(Err(zx::Status::BAD_STATE))
                }
            }
        }
    }

    /// Reads the next datagram that becomes available onto the end of |out|.  Note: Using this
    /// multiple times concurrently is an error and the first one will never complete.
    pub async fn read_datagram<'a>(&'a self, out: &'a mut Vec<u8>) -> Result<usize, zx::Status> {
        await!(poll_fn(move |lw| self.poll_datagram(out, lw)))
    }
}

impl fmt::Debug for Socket {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.0.get_ref().fmt(f)
    }
}

impl AsyncRead for Socket {
    unsafe fn initializer(&self) -> Initializer {
        // This is safe because `zx::Socket::read` does not examine
        // the buffer before reading into it.
        Initializer::nop()
    }

    fn poll_read(&mut self, lw: &LocalWaker, buf: &mut [u8]) -> Poll<io::Result<usize>> {
        self.read_nomut(buf, lw).map_err(Into::into)
    }
}

impl AsyncWrite for Socket {
    fn poll_write(&mut self, lw: &LocalWaker, buf: &[u8]) -> Poll<io::Result<usize>> {
        self.write_nomut(buf, lw).map_err(Into::into)
    }

    fn poll_flush(&mut self, _: &LocalWaker) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_close(&mut self, _: &LocalWaker) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

impl<'a> AsyncRead for &'a Socket {
    unsafe fn initializer(&self) -> Initializer {
        // This is safe because `zx::Socket::read` does not examine
        // the buffer before reading into it.
        Initializer::nop()
    }

    fn poll_read(&mut self, lw: &LocalWaker, buf: &mut [u8]) -> Poll<io::Result<usize>> {
        self.read_nomut(buf, lw).map_err(Into::into)
    }
}

impl<'a> AsyncWrite for &'a Socket {
    fn poll_write(&mut self, lw: &LocalWaker, buf: &[u8]) -> Poll<io::Result<usize>> {
        self.write_nomut(buf, lw).map_err(Into::into)
    }

    fn poll_flush(&mut self, _: &LocalWaker) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_close(&mut self, _: &LocalWaker) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

/// Note: It's probably a good idea to split the socket into read/write halves
/// before using this so you can still write on this socket.
/// Taking two streams of the same socket will not work.
impl Stream for Socket {
    type Item = Result<Vec<u8>, zx::Status>;

    fn poll_next(self: Pin<&mut Self>, lw: &LocalWaker) -> Poll<Option<Self::Item>> {
        let mut res = Vec::<u8>::new();
        match self.poll_datagram(&mut res, lw) {
            Poll::Ready(Ok(_size)) => Poll::Ready(Some(Ok(res))),
            Poll::Ready(Err(e)) => Poll::Ready(Some(Err(e))),
            Poll::Pending => Poll::Pending,
        }
    }
}

impl<'a> Stream for &'a Socket {
    type Item = Result<Vec<u8>, zx::Status>;

    fn poll_next(self: Pin<&mut Self>, lw: &LocalWaker) -> Poll<Option<Self::Item>> {
        let mut res = Vec::<u8>::new();
        match self.poll_datagram(&mut res, lw) {
            Poll::Ready(Ok(_size)) => Poll::Ready(Some(Ok(res))),
            Poll::Ready(Err(e)) => Poll::Ready(Some(Err(e))),
            Poll::Pending => Poll::Pending,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        temp::{TempAsyncReadExt, TempAsyncWriteExt},
        Executor, TimeoutExt, Timer,
    };
    use fuchsia_zircon::prelude::*;
    use futures::future::{FutureExt, TryFutureExt};
    use futures::stream::StreamExt;

    #[test]
    fn can_read_write() {
        let mut exec = Executor::new().unwrap();
        let bytes = &[0, 1, 2, 3];

        let (tx, rx) = zx::Socket::create(zx::SocketOpts::STREAM).unwrap();
        let (tx, rx) = (
            Socket::from_socket(tx).unwrap(),
            Socket::from_socket(rx).unwrap(),
        );

        let receive_future = rx.read_to_end(vec![]).map_ok(|(_socket, buf)| {
            assert_eq!(&*buf, bytes);
        });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let receiver = receive_future.on_timeout(300.millis().after_now(), || panic!("timeout"));

        // Sends a message after the timeout has passed
        let sender = Timer::new(100.millis().after_now())
            .then(|()| tx.write_all(bytes))
            .map_ok(|_tx| ());

        let done = receiver.try_join(sender);
        exec.run_singlethreaded(done).unwrap();
    }

    #[test]
    fn can_read_datagram() {
        let mut exec = Executor::new().unwrap();

        let (one, two) = (&[0, 1], &[2, 3, 4, 5]);

        let (tx, rx) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();
        let rx = Socket::from_socket(rx).unwrap();

        let mut out = vec![50];

        assert!(tx.write(one).is_ok());
        assert!(tx.write(two).is_ok());

        let size = exec.run_singlethreaded(rx.read_datagram(&mut out));

        assert!(size.is_ok());
        assert_eq!(one.len(), size.unwrap());

        assert_eq!([50, 0, 1], out.as_slice());

        let size = exec.run_singlethreaded(rx.read_datagram(&mut out));

        assert!(size.is_ok());
        assert_eq!(two.len(), size.unwrap());

        assert_eq!([50, 0, 1, 2, 3, 4, 5], out.as_slice());
    }

    #[test]
    fn stream_datagram() {
        let mut exec = Executor::new().unwrap();

        let (tx, rx) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();
        let mut rx = Socket::from_socket(rx).unwrap();

        let packets = 20;

        for size in 1..packets + 1 {
            let mut vec = Vec::<u8>::new();
            vec.resize(size, size as u8);
            assert!(tx.write(&vec).is_ok());
        }

        // Close the socket.
        drop(tx);

        let stream_read_fut = async move {
            let mut count = 0;
            while let Some(packet) = await!(rx.next()) {
                let packet = match packet {
                    Ok(bytes) => bytes,
                    Err(zx::Status::PEER_CLOSED) => break,
                    e => panic!("received error from stream: {:?}", e),
                };
                count = count + 1;
                assert_eq!(packet.len(), count);
                assert!(packet.iter().all(|&x| x == count as u8));
            }
            assert_eq!(packets, count);
        };

        exec.run_singlethreaded(stream_read_fut);
    }

}
