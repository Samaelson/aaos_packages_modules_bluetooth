/******************************************************************************
 *
 *  Copyright 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#include "hci/include/packet_fragmenter.h"

#include <gtest/gtest.h>
#include <stdint.h>

#include "AllocationTestHarness.h"
#include "device/include/controller.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "stack/include/bt_hdr.h"
#include "test_stubs.h"

#ifndef HCI_ACL_PREAMBLE_SIZE
#define HCI_ACL_PREAMBLE_SIZE 4
#endif

#ifndef HCI_ISO_PREAMBLE_SIZE
#define HCI_ISO_PREAMBLE_SIZE 4
#endif

DECLARE_TEST_MODES(init, set_data_sizes, no_fragmentation, fragmentation,
                   ble_no_fragmentation, ble_fragmentation,
                   non_acl_passthrough_fragmentation, no_reassembly, reassembly,
                   non_acl_passthrough_reassembly, iso_no_reassembly,
                   iso_reassembly, iso_fragmentation, iso_no_fragmentation);

#define LOCAL_BLE_CONTROLLER_ID 1

static const char* sample_data =
    "At this point they came in sight of thirty forty windmills that there are "
    "on plain, and "
    "as soon as Don Quixote saw them he said to his squire, \"Fortune is "
    "arranging matters "
    "for us better than we could have shaped our desires ourselves, for look "
    "there, friend "
    "Sancho Panza, where thirty or more monstrous giants present themselves, "
    "all of whom I "
    "mean to engage in battle and slay, and with whose spoils we shall begin "
    "to make our "
    "fortunes; for this is righteous warfare, and it is God's good service to "
    "sweep so evil "
    "a breed from off the face of the earth.\"";

static const char* small_sample_data = "\"What giants?\" said Sancho Panza.";
static const uint16_t test_handle_start = (0x1992 & 0xCFFF) | 0x2000;
static const uint16_t test_handle_continuation = (0x1992 & 0xCFFF) | 0x1000;
static const uint16_t test_iso_handle_complete_with_ts =
    (0x0666 | (0x0002 << 12) | (0x0001 << 14));
static const uint16_t test_iso_handle_complete_without_ts =
    (0x0666 | (0x0002 << 12));
static const uint16_t test_iso_handle_start_with_ts = (0x0666 | (0x0001 << 14));
static const uint16_t test_iso_handle_start_without_ts =
    (0x0666);  // Also base handle
static const uint16_t test_iso_handle_continuation = (0x0666 | (0x0001 << 12));
static const uint16_t test_iso_handle_end = (0x0666 | (0x0003 << 12));

static int packet_index;
static unsigned int data_size_sum;

static const packet_fragmenter_t* fragmenter;

static uint32_t iso_timestamp = 0x32122321;
static uint16_t iso_packet_seq = 0x1291;
static bool iso_has_ts = true;

static BT_HDR* manufacture_packet_for_fragmentation(uint16_t event,
                                                    const char* data) {
  uint16_t data_length = strlen(data);
  uint16_t size = data_length;
  if (event == MSG_STACK_TO_HC_HCI_ACL) {
    size += 4;  // 2 for the handle, 2 for the length;
  } else if (event == MSG_STACK_TO_HC_HCI_ISO) {
    size += 8;  // handle (2), length (2), packet_seq (2), sdu_len(2)
    if (iso_has_ts) {
      size += 4;
    }
  }

  BT_HDR* packet = (BT_HDR*)osi_malloc(size + sizeof(BT_HDR));
  packet->len = size;
  packet->offset = 0;
  packet->event = event;
  packet->layer_specific = 0;
  uint8_t* packet_data = packet->data;

  if (event == MSG_STACK_TO_HC_HCI_ACL) {
    UINT16_TO_STREAM(packet_data, test_handle_start);
    UINT16_TO_STREAM(packet_data, data_length);
  } else if (event == MSG_STACK_TO_HC_HCI_ISO) {
    if (iso_has_ts) {
      packet->layer_specific |= BT_ISO_HDR_CONTAINS_TS;
      UINT16_TO_STREAM(packet_data, test_iso_handle_start_with_ts);
      UINT16_TO_STREAM(packet_data, data_length + 8);
      UINT32_TO_STREAM(packet_data, iso_timestamp);
    } else {
      UINT16_TO_STREAM(packet_data, test_iso_handle_start_without_ts);
      UINT16_TO_STREAM(packet_data, data_length + 4);
    }
    UINT16_TO_STREAM(packet_data, iso_packet_seq);
    UINT16_TO_STREAM(packet_data, data_length);
  }

  memcpy(packet_data, data, data_length);
  return packet;
}

static void expect_packet_fragmented(uint16_t event, int max_acl_data_size,
                                     BT_HDR* packet, const char* expected_data,
                                     bool send_complete) {
  uint8_t* data = packet->data + packet->offset;
  int expected_data_offset;
  int length_to_check;

  if (event == MSG_STACK_TO_HC_HCI_ACL) {
    uint16_t handle;
    uint16_t length;
    STREAM_TO_UINT16(handle, data);
    STREAM_TO_UINT16(length, data);

    if (packet_index == 0)
      EXPECT_EQ(test_handle_start, handle);
    else
      EXPECT_EQ(test_handle_continuation, handle);

    int length_remaining = strlen(expected_data) - data_size_sum;
    int packet_data_length = packet->len - HCI_ACL_PREAMBLE_SIZE;
    EXPECT_EQ(packet_data_length, length);

    if (length_remaining > max_acl_data_size)
      EXPECT_EQ(max_acl_data_size, packet_data_length);

    length_to_check = packet_data_length;
    expected_data_offset = packet_index * max_acl_data_size;
    packet_index++;
  } else if (event == MSG_STACK_TO_HC_HCI_ISO) {
    uint16_t handle;
    uint16_t length;

    STREAM_TO_UINT16(handle, data);
    STREAM_TO_UINT16(length, data);

    int length_remaining = strlen(expected_data) - data_size_sum;
    int packet_data_length = packet->len - HCI_ISO_PREAMBLE_SIZE;

    if (packet_index == 0) {
      uint8_t hdr_size = 8;  // ts, packet_seq, len

      if (iso_has_ts) {
        uint32_t timestamp;
        STREAM_TO_UINT32(timestamp, data);
        ASSERT_EQ(timestamp, iso_timestamp);

        if (send_complete)
          ASSERT_EQ(test_iso_handle_complete_with_ts, handle);
        else
          ASSERT_EQ(test_iso_handle_start_with_ts, handle);
      } else {
        if (send_complete)
          ASSERT_EQ(test_iso_handle_complete_without_ts, handle);
        else
          ASSERT_EQ(test_iso_handle_start_without_ts, handle);
        hdr_size -= 4;
      }

      uint16_t packet_seq;
      STREAM_TO_UINT16(packet_seq, data);
      ASSERT_EQ(packet_seq, iso_packet_seq);

      uint16_t iso_sdu_length;
      STREAM_TO_UINT16(iso_sdu_length, data);

      ASSERT_EQ(iso_sdu_length, strlen(expected_data));

      length_to_check = packet_data_length - hdr_size;
    } else {
      if (!send_complete)
        ASSERT_EQ(test_iso_handle_continuation, handle);
      else
        ASSERT_EQ(test_iso_handle_end, handle);

      length_to_check = packet_data_length;
    }

    if (length_remaining > max_acl_data_size)
      ASSERT_EQ(max_acl_data_size, packet_data_length);

    expected_data_offset = packet_index * max_acl_data_size;
    if (expected_data_offset > 0) {
      if (iso_has_ts) {
        expected_data_offset -= 8;
      } else {
        expected_data_offset -= 4;
      }
    }
    packet_index++;
  } else {
    length_to_check = strlen(expected_data);
    expected_data_offset = 0;
  }

  for (int i = 0; i < length_to_check; i++) {
    EXPECT_EQ(expected_data[expected_data_offset + i], data[i]);
    data_size_sum++;
  }

  if (event == MSG_STACK_TO_HC_HCI_ISO || event == MSG_STACK_TO_HC_HCI_ACL)
    EXPECT_TRUE(send_complete == (data_size_sum == strlen(expected_data)));

  if (send_complete) osi_free(packet);
}

static void manufacture_acl_packet_and_then_reassemble(uint16_t event,
                                                       uint16_t acl_size,
                                                       const char* data) {
  uint16_t data_length = strlen(data);
  uint16_t total_length = data_length + 2;  // 2 for l2cap length;
  uint16_t length_sent = 0;
  uint16_t l2cap_length = data_length - 2;  // l2cap length field, 2 for the
                                            // pretend channel id borrowed
                                            // from the data

  do {
    int length_to_send = (length_sent + (acl_size - 4) < total_length)
                             ? (acl_size - 4)
                             : (total_length - length_sent);
    BT_HDR* packet = (BT_HDR*)osi_malloc(length_to_send + 4 + sizeof(BT_HDR));
    packet->len = length_to_send + 4;
    packet->offset = 0;
    packet->event = event;
    packet->layer_specific = 0;

    uint8_t* packet_data = packet->data;
    if (length_sent == 0) {  // first packet
      UINT16_TO_STREAM(packet_data, test_handle_start);
      UINT16_TO_STREAM(packet_data, length_to_send);
      UINT16_TO_STREAM(packet_data, l2cap_length);
      memcpy(packet_data, data, length_to_send - 2);
    } else {
      UINT16_TO_STREAM(packet_data, test_handle_continuation);
      UINT16_TO_STREAM(packet_data, length_to_send);
      memcpy(packet_data, data + length_sent - 2, length_to_send);
    }

    length_sent += length_to_send;
    fragmenter->reassemble_and_dispatch(packet);
  } while (length_sent < total_length);
}

static void manufacture_iso_packet_and_then_reassemble(uint16_t event,
                                                       uint16_t iso_size,
                                                       const char* data) {
  uint16_t data_length = strlen(data);
  uint16_t total_length;
  uint16_t length_sent = 0;
  uint16_t iso_length = data_length;
  BT_HDR* packet;
  uint8_t* packet_data;
  uint8_t hdr_size = 4;  // packet seq, sdu len

  // ISO length (2), packet seq (2), optional timestamp (4)
  total_length = data_length + 4;
  if (iso_has_ts) total_length += 4;

  do {
    int length_to_send = (length_sent + (iso_size - 4) < total_length)
                             ? (iso_size - 4)
                             : (total_length - length_sent);

    packet = (BT_HDR*)osi_malloc(length_to_send + 4 + sizeof(BT_HDR));
    packet_data = packet->data;
    packet->len = length_to_send + 4;
    packet->offset = 0;
    packet->event = event;
    packet->layer_specific = 0;

    bool is_complete = length_to_send == total_length;

    if (length_sent == 0) {  // first packet
      if (iso_has_ts) {
        hdr_size += 4;
        if (is_complete) {
          UINT16_TO_STREAM(packet_data, test_iso_handle_complete_with_ts);
        } else {
          UINT16_TO_STREAM(packet_data, test_iso_handle_start_with_ts);
        }
      } else {
        if (is_complete) {
          UINT16_TO_STREAM(packet_data, test_iso_handle_complete_without_ts);
        } else {
          UINT16_TO_STREAM(packet_data, test_iso_handle_start_without_ts);
        }
      }

      UINT16_TO_STREAM(packet_data, length_to_send);

      if (iso_has_ts) {
        UINT32_TO_STREAM(packet_data, iso_timestamp);
      }

      UINT16_TO_STREAM(packet_data, iso_packet_seq);
      UINT16_TO_STREAM(packet_data, iso_length);
      memcpy(packet_data, data, length_to_send - hdr_size);
    } else {
      if (length_sent + length_to_send == total_length) {
        UINT16_TO_STREAM(packet_data, test_iso_handle_end);
      } else {
        UINT16_TO_STREAM(packet_data, test_iso_handle_continuation);
      }
      UINT16_TO_STREAM(packet_data, length_to_send);
      memcpy(packet_data, data + length_sent - hdr_size, length_to_send);
    }

    length_sent += length_to_send;
    fragmenter->reassemble_and_dispatch(packet);
  } while (length_sent < total_length);
}

static void manufacture_packet_and_then_reassemble(uint16_t event,
                                                   uint16_t packet_size,
                                                   const char* data) {
  uint16_t data_length = strlen(data);

  if (event == MSG_HC_TO_STACK_HCI_ACL) {
    manufacture_acl_packet_and_then_reassemble(event, packet_size, data);
  } else if (event == MSG_HC_TO_STACK_HCI_ISO) {
    manufacture_iso_packet_and_then_reassemble(event, packet_size, data);
  } else {
    BT_HDR* packet = (BT_HDR*)osi_malloc(data_length + sizeof(BT_HDR));
    packet->len = data_length;
    packet->offset = 0;
    packet->event = event;
    packet->layer_specific = 0;
    memcpy(packet->data, data, data_length);

    fragmenter->reassemble_and_dispatch(packet);
  }
}

static void expect_packet_reassembled(uint16_t event, BT_HDR* packet,
                                      const char* expected_data) {
  uint16_t expected_data_length = strlen(expected_data);
  uint8_t* data = packet->data + packet->offset;

  if (event == MSG_HC_TO_STACK_HCI_ACL) {
    uint16_t handle;
    uint16_t length;
    uint16_t l2cap_length;
    STREAM_TO_UINT16(handle, data);
    STREAM_TO_UINT16(length, data);
    STREAM_TO_UINT16(l2cap_length, data);

    EXPECT_EQ(test_handle_start, handle);
    EXPECT_EQ(expected_data_length + 2, length);
    EXPECT_EQ(expected_data_length - 2,
              l2cap_length);  // -2 for the pretend channel id
  }

  for (int i = 0; i < expected_data_length; i++) {
    EXPECT_EQ(expected_data[i], data[i]);
    data_size_sum++;
  }

  osi_free(packet);
}

static void expect_packet_reassembled_iso(uint16_t event, BT_HDR* packet,
                                          const char* expected_data,
                                          uint32_t expected_timestamp,
                                          uint16_t expected_packet_seq,
                                          bool is_complete = false) {
  uint16_t expected_data_length = strlen(expected_data);
  uint8_t* data = packet->data;
  uint8_t hdr_size = 8;
  uint16_t handle;
  uint16_t length;
  uint16_t iso_length;
  uint32_t timestamp;
  uint16_t packet_seq;

  ASSERT_EQ(event, MSG_HC_TO_STACK_HCI_ISO);

  STREAM_TO_UINT16(handle, data);
  STREAM_TO_UINT16(length, data);
  if (iso_has_ts) {
    STREAM_TO_UINT32(timestamp, data);
    ASSERT_TRUE((packet->layer_specific & BT_ISO_HDR_CONTAINS_TS) != 0);
    ASSERT_EQ(timestamp, expected_timestamp);
    ASSERT_EQ(is_complete ? test_iso_handle_complete_with_ts
                          : test_iso_handle_start_with_ts,
              handle);
  } else {
    ASSERT_TRUE((packet->layer_specific & BT_ISO_HDR_CONTAINS_TS) == 0);
    ASSERT_EQ(is_complete ? test_iso_handle_complete_without_ts
                          : test_iso_handle_start_without_ts,
              handle);
    hdr_size -= 4;
  }

  ASSERT_EQ(expected_data_length + hdr_size, length);

  STREAM_TO_UINT16(packet_seq, data);
  ASSERT_EQ(packet_seq, expected_packet_seq);

  STREAM_TO_UINT16(iso_length, data);
  ASSERT_EQ(expected_data_length, iso_length);

  for (int i = 0; i < expected_data_length; i++) {
    ASSERT_EQ(expected_data[i], data[i]);
    data_size_sum++;
  }

  osi_free(packet);
}

STUB_FUNCTION(void, fragmented_callback, (BT_HDR * packet, bool send_complete))
DURING(no_fragmentation) AT_CALL(0) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_ACL, 42, packet,
                           small_sample_data, send_complete);
  return;
}

DURING(fragmentation) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_ACL, 10, packet, sample_data,
                           send_complete);
  return;
}

DURING(ble_no_fragmentation) AT_CALL(0) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_ACL, 42, packet,
                           small_sample_data, send_complete);
  return;
}

DURING(ble_fragmentation) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_ACL, 10, packet, sample_data,
                           send_complete);
  return;
}

DURING(iso_fragmentation) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_ISO, 10, packet, sample_data,
                           send_complete);
  return;
}

DURING(iso_no_fragmentation) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_ISO, 42, packet,
                           small_sample_data, send_complete);
  return;
}

DURING(non_acl_passthrough_fragmentation) AT_CALL(0) {
  expect_packet_fragmented(MSG_STACK_TO_HC_HCI_CMD, 10, packet, sample_data,
                           send_complete);
  return;
}

UNEXPECTED_CALL;
}

STUB_FUNCTION(void, reassembled_callback, (BT_HDR * packet))
DURING(no_reassembly) AT_CALL(0) {
  expect_packet_reassembled(MSG_HC_TO_STACK_HCI_ACL, packet, small_sample_data);
  return;
}

DURING(reassembly) AT_CALL(0) {
  expect_packet_reassembled(MSG_HC_TO_STACK_HCI_ACL, packet, sample_data);
  return;
}

DURING(iso_reassembly) AT_CALL(0) {
  expect_packet_reassembled_iso(MSG_HC_TO_STACK_HCI_ISO, packet, sample_data,
                                iso_timestamp, iso_packet_seq);
  return;
}

DURING(iso_no_reassembly) AT_CALL(0) {
  expect_packet_reassembled_iso(MSG_HC_TO_STACK_HCI_ISO, packet,
                                small_sample_data, iso_timestamp,
                                iso_packet_seq, true);
  return;
}

DURING(non_acl_passthrough_reassembly) AT_CALL(0) {
  expect_packet_reassembled(MSG_HC_TO_STACK_HCI_EVT, packet, sample_data);
  return;
}

UNEXPECTED_CALL;
}

STUB_FUNCTION(void, transmit_finished_callback,
              (UNUSED_ATTR BT_HDR * packet,
               UNUSED_ATTR bool sent_all_fragments))
UNEXPECTED_CALL;
}

STUB_FUNCTION(uint16_t, get_acl_data_size_classic, (void))
DURING(no_fragmentation, non_acl_passthrough_fragmentation, no_reassembly)
return 42;
DURING(fragmentation) return 10;
DURING(no_reassembly) return 1337;

UNEXPECTED_CALL;
return 0;
}

STUB_FUNCTION(uint16_t, get_acl_data_size_ble, (void))
DURING(ble_no_fragmentation) return 42;
DURING(ble_fragmentation) return 10;

UNEXPECTED_CALL;
return 0;
}

STUB_FUNCTION(uint16_t, get_iso_data_size, (void))
DURING(iso_no_fragmentation) return 42;
DURING(iso_fragmentation) return 10;

UNEXPECTED_CALL;
return 0;
}

static void reset_for(TEST_MODES_T next) {
  RESET_CALL_COUNT(fragmented_callback);
  RESET_CALL_COUNT(reassembled_callback);
  RESET_CALL_COUNT(transmit_finished_callback);
  RESET_CALL_COUNT(get_acl_data_size_classic);
  RESET_CALL_COUNT(get_acl_data_size_ble);
  RESET_CALL_COUNT(get_iso_data_size);
  CURRENT_TEST_MODE = next;
}

class PacketFragmenterTest : public AllocationTestHarness {
 protected:
  void SetUp() override {
    AllocationTestHarness::SetUp();
    fragmenter =
        packet_fragmenter_get_test_interface(&controller, &allocator_malloc);

    packet_index = 0;
    data_size_sum = 0;

    callbacks.fragmented = fragmented_callback;
    callbacks.reassembled = reassembled_callback;
    callbacks.transmit_finished = transmit_finished_callback;
    controller.get_acl_data_size_classic = get_acl_data_size_classic;
    controller.get_acl_data_size_ble = get_acl_data_size_ble;
    controller.get_iso_data_size = get_iso_data_size;

    reset_for(init);
    fragmenter->init(&callbacks);
  }

  void TearDown() override {
    fragmenter->cleanup();
    AllocationTestHarness::TearDown();
  }

  controller_t controller;
  packet_fragmenter_callbacks_t callbacks;
};

TEST_F(PacketFragmenterTest, test_no_fragment_necessary) {
  reset_for(no_fragmentation);
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ACL,
                                                        small_sample_data);
  fragmenter->fragment_and_dispatch(packet);

  EXPECT_EQ(strlen(small_sample_data), data_size_sum);
  EXPECT_CALL_COUNT(fragmented_callback, 1);
}

TEST_F(PacketFragmenterTest, test_fragment_necessary) {
  reset_for(fragmentation);
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ACL,
                                                        sample_data);
  fragmenter->fragment_and_dispatch(packet);

  EXPECT_EQ(strlen(sample_data), data_size_sum);
}

TEST_F(PacketFragmenterTest, test_ble_no_fragment_necessary) {
  reset_for(ble_no_fragmentation);
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ACL,
                                                        small_sample_data);
  packet->event |= LOCAL_BLE_CONTROLLER_ID;
  fragmenter->fragment_and_dispatch(packet);

  EXPECT_EQ(strlen(small_sample_data), data_size_sum);
  EXPECT_CALL_COUNT(fragmented_callback, 1);
}

TEST_F(PacketFragmenterTest, test_ble_fragment_necessary) {
  reset_for(ble_fragmentation);
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ACL,
                                                        sample_data);
  packet->event |= LOCAL_BLE_CONTROLLER_ID;
  fragmenter->fragment_and_dispatch(packet);

  EXPECT_EQ(strlen(sample_data), data_size_sum);
}

TEST_F(PacketFragmenterTest, test_non_acl_passthrough_fragmentation) {
  reset_for(non_acl_passthrough_fragmentation);
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_CMD,
                                                        sample_data);
  fragmenter->fragment_and_dispatch(packet);

  EXPECT_EQ(strlen(sample_data), data_size_sum);
  EXPECT_CALL_COUNT(fragmented_callback, 1);
}

TEST_F(PacketFragmenterTest, test_no_reassembly_necessary) {
  reset_for(no_reassembly);
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_ACL, 1337,
                                         small_sample_data);

  EXPECT_EQ(strlen(small_sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}

TEST_F(PacketFragmenterTest, test_reassembly_necessary) {
  reset_for(reassembly);
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_ACL, 42,
                                         sample_data);

  EXPECT_EQ(strlen(sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}

TEST_F(PacketFragmenterTest, test_non_acl_passthrough_reasseembly) {
  reset_for(non_acl_passthrough_reassembly);
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_EVT, 42,
                                         sample_data);

  EXPECT_EQ(strlen(sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}

TEST_F(PacketFragmenterTest, test_iso_fragment_necessary) {
  reset_for(iso_fragmentation);
  iso_has_ts = true;

  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ISO,
                                                        sample_data);
  packet->event |= LOCAL_BLE_CONTROLLER_ID;
  fragmenter->fragment_and_dispatch(packet);

  ASSERT_EQ(strlen(sample_data), data_size_sum);
}

TEST_F(PacketFragmenterTest, test_iso_no_fragment_necessary) {
  reset_for(iso_no_fragmentation);
  iso_has_ts = true;

  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ISO,
                                                        small_sample_data);
  packet->event |= LOCAL_BLE_CONTROLLER_ID;
  fragmenter->fragment_and_dispatch(packet);

  ASSERT_EQ(strlen(small_sample_data), data_size_sum);
}

TEST_F(PacketFragmenterTest, test_iso_fragment_necessary_no_ts) {
  reset_for(iso_fragmentation);
  iso_has_ts = false;
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ISO,
                                                        sample_data);
  packet->event |= LOCAL_BLE_CONTROLLER_ID;
  fragmenter->fragment_and_dispatch(packet);

  ASSERT_EQ(strlen(sample_data), data_size_sum);
}

TEST_F(PacketFragmenterTest, test_iso_no_fragment_necessary_no_ts) {
  reset_for(iso_no_fragmentation);
  iso_has_ts = false;
  BT_HDR* packet = manufacture_packet_for_fragmentation(MSG_STACK_TO_HC_HCI_ISO,
                                                        small_sample_data);
  packet->event |= LOCAL_BLE_CONTROLLER_ID;
  fragmenter->fragment_and_dispatch(packet);

  ASSERT_EQ(strlen(small_sample_data), data_size_sum);
}

TEST_F(PacketFragmenterTest, test_iso_no_reassembly_necessary) {
  reset_for(iso_no_reassembly);
  iso_has_ts = true;
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_ISO, 50,
                                         small_sample_data);

  ASSERT_EQ(strlen(small_sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}

TEST_F(PacketFragmenterTest, test_iso_reassembly_necessary) {
  reset_for(iso_reassembly);
  iso_has_ts = true;
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_ISO, 42,
                                         sample_data);

  ASSERT_EQ(strlen(sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}

TEST_F(PacketFragmenterTest, test_iso_no_reassembly_necessary_no_ts) {
  reset_for(iso_no_reassembly);
  iso_has_ts = false;
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_ISO, (42 + 4),
                                         small_sample_data);

  ASSERT_EQ(strlen(small_sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}

TEST_F(PacketFragmenterTest, test_iso_reassembly_necessary_no_ts) {
  reset_for(iso_reassembly);
  iso_has_ts = false;
  manufacture_packet_and_then_reassemble(MSG_HC_TO_STACK_HCI_ISO, 42,
                                         sample_data);

  ASSERT_EQ(strlen(sample_data), data_size_sum);
  EXPECT_CALL_COUNT(reassembled_callback, 1);
}
