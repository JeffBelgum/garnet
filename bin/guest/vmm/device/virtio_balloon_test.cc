// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <virtio/balloon.h>

#include "garnet/bin/guest/vmm/device/test_with_device.h"
#include "garnet/bin/guest/vmm/device/virtio_queue_fake.h"

static constexpr char kVirtioBalloonUrl[] = "virtio_balloon";
static constexpr uint16_t kNumQueues = 3;
static constexpr uint16_t kQueueSize = 16;

class VirtioBalloonTest : public TestWithDevice {
 protected:
  VirtioBalloonTest()
      : inflate_queue_(phys_mem_, PAGE_SIZE * kNumQueues, kQueueSize),
        deflate_queue_(phys_mem_, inflate_queue_.end(), kQueueSize),
        stats_queue_(phys_mem_, deflate_queue_.end(), kQueueSize) {}

  void SetUp() override {
    // Launch device process.
    fuchsia::guest::device::StartInfo start_info;
    zx_status_t status =
        LaunchDevice(kVirtioBalloonUrl, stats_queue_.end(), &start_info);
    ASSERT_EQ(ZX_OK, status);

    // Start device execution.
    services.ConnectToService(balloon_.NewRequest());
    status = balloon_->Start(std::move(start_info), false /* demand_page */);
    ASSERT_EQ(ZX_OK, status);
    status = balloon_->Ready(VIRTIO_BALLOON_F_STATS_VQ);
    ASSERT_EQ(ZX_OK, status);

    // Configure device queues.
    VirtioQueueFake* queues[kNumQueues] = {&inflate_queue_, &deflate_queue_,
                                           &stats_queue_};
    for (size_t i = 0; i < kNumQueues; i++) {
      auto q = queues[i];
      q->Configure(PAGE_SIZE * i, PAGE_SIZE);
      status = balloon_->ConfigureQueue(i, q->size(), q->desc(), q->avail(),
                                        q->used());
      ASSERT_EQ(ZX_OK, status);
    }
  }

  fuchsia::guest::device::VirtioBalloonSyncPtr balloon_;
  VirtioQueueFake inflate_queue_;
  VirtioQueueFake deflate_queue_;
  VirtioQueueFake stats_queue_;
};

TEST_F(VirtioBalloonTest, Inflate) {
  uint32_t pfns[] = {0, 1, 2};
  zx_status_t status = DescriptorChainBuilder(inflate_queue_)
                           .AppendReadableDescriptor(pfns, sizeof(pfns))
                           .Build();
  ASSERT_EQ(ZX_OK, status);

  status = balloon_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);
}

TEST_F(VirtioBalloonTest, Deflate) {
  uint32_t pfns[] = {2, 1, 0};
  zx_status_t status = DescriptorChainBuilder(deflate_queue_)
                           .AppendReadableDescriptor(pfns, sizeof(pfns))
                           .Build();
  ASSERT_EQ(ZX_OK, status);

  status = balloon_->NotifyQueue(1);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);
}

TEST_F(VirtioBalloonTest, StatsShouldWait) {
  zx_status_t stats_status;
  fidl::VectorPtr<fuchsia::guest::MemStat> mem_stats;
  zx_status_t status = balloon_->GetMemStats(&stats_status, &mem_stats);
  ASSERT_EQ(ZX_OK, status);
  ASSERT_EQ(ZX_ERR_SHOULD_WAIT, stats_status);
}