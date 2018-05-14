// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ESCHER_VK_TEXTURE_H_
#define LIB_ESCHER_VK_TEXTURE_H_

#include "lib/escher/forward_declarations.h"
#include "lib/escher/resources/resource.h"
#include "lib/escher/vk/image.h"

namespace escher {

class Texture : public Resource {
 public:
  static const ResourceTypeInfo kTypeInfo;
  const ResourceTypeInfo& type_info() const override { return kTypeInfo; }

  // Construct a new Texture, which encapsulates a newly-created VkImageView and
  // VkSampler.  |aspect_mask| is used to create the VkImageView, and |filter|
  // and |use_unnormalized_coordinates| are used to create the VkSampler.
  // |resource_recycler| guarantees that the underlying Vulkan resources are not
  // destroyed while still referenced by a pending command buffer.
  Texture(ResourceRecycler* resource_recycler,
          ImagePtr image,
          vk::Filter filter,
          vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor,
          bool use_unnormalized_coordinates = false);
  ~Texture() override;

  static TexturePtr New(
      ResourceRecycler* resource_recycler,
      ImagePtr image,
      vk::Filter filter,
      vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor,
      bool use_unnormalized_coordinates = false);

  const ImagePtr& image() const { return image_; }
  vk::Image vk_image() const { return image_->vk(); }
  vk::ImageView vk_image_view() const { return image_view_; }
  vk::Sampler vk_sampler() const { return sampler_; }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  // TODO(ES-44): Deprecated.  Use vk_image_view() and vk_sampler() instead.
  vk::ImageView image_view() const { return image_view_; }
  vk::Sampler sampler() const { return sampler_; }

 private:
  ImagePtr image_;
  vk::Device device_;
  vk::ImageView image_view_;
  vk::Sampler sampler_;
  uint32_t width_;
  uint32_t height_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Texture);
};

typedef fxl::RefPtr<Texture> TexturePtr;

}  // namespace escher

#endif  // LIB_ESCHER_VK_TEXTURE_H_
