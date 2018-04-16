// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"log"

	"app/context"
	"fidl/bindings2"

	"syscall/zx"

	"fuchsia/go/echo2"
)

type echoImpl struct{}

func (echo *echoImpl) EchoString(inValue *string) (outValue *string, err error) {
	log.Printf("server: %s\n", *inValue)
	return inValue, nil
}

func main() {
	echoService := &bindings2.BindingSet{}
	c := context.CreateFromStartupInfo()
	c.OutgoingService.AddService(echo2.EchoName, func(c zx.Channel) error {
		return echoService.Add(&echo2.EchoStub{
			Impl: &echoImpl{},
		}, c)
	})
	c.Serve()
	go bindings2.Serve()

	select {}
}
