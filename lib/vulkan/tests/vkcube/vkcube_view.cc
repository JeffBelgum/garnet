// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vkcube_view.h"
#include "garnet/public/lib/ui/scenic/fidl_helpers.h"
#include "lib/ui/geometry/cpp/geometry_util.h"

VkCubeView::VkCubeView(
    views_v1::ViewManagerPtr view_manager,
    fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
    std::function<
        void(float width, float height,
             fidl::InterfaceHandle<images::ImagePipe> interface_request)>
        resize_callback)
    : BaseView(std::move(view_manager), std::move(view_owner_request),
               "vkcube"),
      pane_node_(session()),
      resize_callback_(resize_callback) {}

VkCubeView::~VkCubeView() {}

void VkCubeView::OnSceneInvalidated(
    images::PresentationInfo presentation_info) {
  if (!has_metrics()) return;
  if (size_ == logical_size() && physical_size_ == physical_size()) return;

  size_ = logical_size();
  physical_size_ = physical_size();

  scenic_lib::Rectangle pane_shape(session(), logical_size().width,
                                   logical_size().height);
  scenic_lib::Material pane_material(session());

  pane_node_.SetShape(pane_shape);
  pane_node_.SetMaterial(pane_material);
  pane_node_.SetTranslation(logical_size().width * 0.5,
                            logical_size().height * 0.5, 0);
  parent_node().AddChild(pane_node_);

  zx::channel endpoint0;
  zx::channel endpoint1;
  zx::channel::create(0, &endpoint0, &endpoint1);

  uint32_t image_pipe_id = session()->AllocResourceId();
  session()->Enqueue(scenic_lib::NewCreateImagePipeCommand(
      image_pipe_id,
      fidl::InterfaceRequest<images::ImagePipe>(std::move(endpoint1))));
  pane_material.SetTexture(image_pipe_id);
  session()->ReleaseResource(image_pipe_id);

  // No need to Present on session; base_view will present after calling
  // OnSceneInvalidated.

  resize_callback_(
      physical_size().width, physical_size().height,
      fidl::InterfaceHandle<images::ImagePipe>(std::move(endpoint0)));
}
