/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  This file contains the main L2CAP entry points
 *
 ******************************************************************************/

#define LOG_TAG "bt_l2c_main"

#include <string.h>

#include "bt_target.h"
#include "gd/hal/snoop_logger.h"
#include "hcimsgs.h"  // HCID_GET_
#include "main/shim/shim.h"
#include "osi/include/allocator.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/l2c_api.h"
#include "stack/include/l2cdefs.h"
#include "stack/l2cap/l2c_int.h"

/******************************************************************************/
/*            L O C A L    F U N C T I O N     P R O T O T Y P E S            */
/******************************************************************************/
static void process_l2cap_cmd(tL2C_LCB* p_lcb, uint8_t* p, uint16_t pkt_len);

/******************************************************************************/
/*               G L O B A L      L 2 C A P       D A T A                     */
/******************************************************************************/
tL2C_CB l2cb;

/*******************************************************************************
 *
 * Function         l2c_rcv_acl_data
 *
 * Description      This function is called from the HCI Interface when an ACL
 *                  data packet is received.
 *
 * Returns          void
 *
 ******************************************************************************/
void l2c_rcv_acl_data(BT_HDR* p_msg) {
  uint8_t* p = (uint8_t*)(p_msg + 1) + p_msg->offset;

  /* Extract the handle */
  uint16_t handle;
  STREAM_TO_UINT16(handle, p);
  uint8_t pkt_type = HCID_GET_EVENT(handle);
  handle = HCID_GET_HANDLE(handle);

  /* Since the HCI Transport is putting segmented packets back together, we */
  /* should never get a valid packet with the type set to "continuation"    */
  if (pkt_type == L2CAP_PKT_CONTINUE) {
    L2CAP_TRACE_WARNING("L2CAP - received packet continuation");
    osi_free(p_msg);
    return;
  }

  uint16_t hci_len;
  STREAM_TO_UINT16(hci_len, p);
  if (hci_len < L2CAP_PKT_OVERHEAD || hci_len != p_msg->len - 4) {
    /* Remote-declared packet size must match HCI_ACL size - ACL header (4) */
    L2CAP_TRACE_WARNING("L2CAP - got incorrect hci header");
    osi_free(p_msg);
    return;
  }

  uint16_t l2cap_len, rcv_cid;
  STREAM_TO_UINT16(l2cap_len, p);
  STREAM_TO_UINT16(rcv_cid, p);

  /* Find the LCB based on the handle */
  tL2C_LCB* p_lcb = l2cu_find_lcb_by_handle(handle);
  if (!p_lcb) {
    LOG_ERROR("L2CAP - rcvd ACL for unknown handle:%d ls:%d cid:%d", handle,
              p_msg->layer_specific, rcv_cid);
    osi_free(p_msg);
    return;
  }

  /* Update the buffer header */
  p_msg->offset += 4;

  /* for BLE channel, always notify connection when ACL data received on the
   * link */
  if (p_lcb && p_lcb->transport == BT_TRANSPORT_LE &&
      p_lcb->link_state != LST_DISCONNECTING) {
    /* only process fixed channel data as channel open indication when link is
     * not in disconnecting mode */
    l2cble_notify_le_connection(p_lcb->remote_bd_addr);
  }

  /* Find the CCB for this CID */
  tL2C_CCB* p_ccb = NULL;
  if (rcv_cid >= L2CAP_BASE_APPL_CID) {
    p_ccb = l2cu_find_ccb_by_cid(p_lcb, rcv_cid);
    if (!p_ccb) {
      L2CAP_TRACE_WARNING("L2CAP - unknown CID: 0x%04x", rcv_cid);
      osi_free(p_msg);
      return;
    }
  }

  p_msg->len = hci_len - L2CAP_PKT_OVERHEAD;
  p_msg->offset += L2CAP_PKT_OVERHEAD;

  if (l2cap_len != p_msg->len) {
    L2CAP_TRACE_WARNING("L2CAP - bad length in pkt. Exp: %d  Act: %d",
                        l2cap_len, p_msg->len);
    osi_free(p_msg);
    return;
  }

  /* Send the data through the channel state machine */
  if (rcv_cid == L2CAP_SIGNALLING_CID) {
    process_l2cap_cmd(p_lcb, p, l2cap_len);
    osi_free(p_msg);
    return;
  }

  if (rcv_cid == L2CAP_CONNECTIONLESS_CID) {
    /* process_connectionless_data (p_lcb); */
    osi_free(p_msg);
    return;
  }

  if (rcv_cid == L2CAP_BLE_SIGNALLING_CID) {
    l2cble_process_sig_cmd(p_lcb, p, l2cap_len);
    osi_free(p_msg);
    return;
  }

  if ((rcv_cid >= L2CAP_FIRST_FIXED_CHNL) &&
      (rcv_cid <= L2CAP_LAST_FIXED_CHNL) &&
      (l2cb.fixed_reg[rcv_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedData_Cb !=
       NULL)) {
    /* only process fixed channel data when link is open or wait for data
     * indication */
    if (!p_lcb || p_lcb->link_state == LST_DISCONNECTING ||
        !l2cu_initialize_fixed_ccb(p_lcb, rcv_cid)) {
      osi_free(p_msg);
      return;
    }

    /* If no CCB for this channel, allocate one */
    p_ccb = p_lcb->p_fixed_ccbs[rcv_cid - L2CAP_FIRST_FIXED_CHNL];
    p_ccb->metrics.rx(p_msg->len);

    if (p_ccb->peer_cfg.fcr.mode != L2CAP_FCR_BASIC_MODE)
      l2c_fcr_proc_pdu(p_ccb, p_msg);
    else
      (*l2cb.fixed_reg[rcv_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedData_Cb)(
          rcv_cid, p_lcb->remote_bd_addr, p_msg);
    return;
  }

  if (!p_ccb) {
    osi_free(p_msg);
    return;
  }

  if (p_lcb->transport == BT_TRANSPORT_LE) {
    l2c_lcc_proc_pdu(p_ccb, p_msg);

    /* The remote device has one less credit left */
    --p_ccb->remote_credit_count;

    /* If the credits left on the remote device are getting low, send some */
    if (p_ccb->remote_credit_count <= L2CA_LeCreditThreshold()) {
      uint16_t credits = L2CA_LeCreditDefault() - p_ccb->remote_credit_count;
      p_ccb->remote_credit_count = L2CA_LeCreditDefault();

      /* Return back credits */
      l2c_csm_execute(p_ccb, L2CEVT_L2CA_SEND_FLOW_CONTROL_CREDIT, &credits);
    }
  } else {
    /* Basic mode packets go straight to the state machine */
    if (p_ccb->peer_cfg.fcr.mode == L2CAP_FCR_BASIC_MODE)
      l2c_csm_execute(p_ccb, L2CEVT_L2CAP_DATA, p_msg);
    else {
      /* eRTM or streaming mode, so we need to validate states first */
      if ((p_ccb->chnl_state == CST_OPEN) || (p_ccb->chnl_state == CST_CONFIG))
        l2c_fcr_proc_pdu(p_ccb, p_msg);
      else
        osi_free(p_msg);
    }
  }
}

/*******************************************************************************
 *
 * Function         process_l2cap_cmd
 *
 * Description      This function is called when a packet is received on the
 *                  L2CAP signalling CID
 *
 * Returns          void
 *
 ******************************************************************************/
static void process_l2cap_cmd(tL2C_LCB* p_lcb, uint8_t* p, uint16_t pkt_len) {
  tL2C_CONN_INFO con_info;
  tL2C_RCB* p_rcb;

  /* if l2cap command received in CID 1 on top of an LE link, ignore this
   * command */
  if (p_lcb->transport == BT_TRANSPORT_LE) {
    LOG_INFO("Dropping data on CID 1 for LE link");
    return;
  }

  /* Reject the packet if it exceeds the default Signalling Channel MTU */
  bool pkt_size_rej = false;
  if (pkt_len > L2CAP_DEFAULT_MTU) {
    /* Core Spec requires a single response to the first command found in a
     * multi-command L2cap packet.  If only responses in the packet, then it
     * will be ignored. Here we simply mark the bad packet and decide which cmd
     * ID to reject later */
    pkt_size_rej = true;
    LOG_WARN("Signaling pkt_len=%d exceeds MTU size %d", pkt_len,
             L2CAP_DEFAULT_MTU);
  }

  uint8_t* p_next_cmd = p;
  uint8_t* p_pkt_end = p + pkt_len;
  uint8_t last_id = 0;
  bool first_cmd = true;

  tL2CAP_CFG_INFO cfg_info;
  memset(&cfg_info, 0, sizeof(cfg_info));

  /* An L2CAP packet may contain multiple commands */
  while (true) {
    /* Smallest command is 4 bytes */
    p = p_next_cmd;
    if (p > (p_pkt_end - 4)) {
      /* Reject to the previous endpoint if reliable channel is being used.
       * This is required in L2CAP/COS/CED/BI-02-C */
      if (!first_cmd &&
          (cfg_info.fcr.mode == L2CAP_FCR_BASIC_MODE ||
           cfg_info.fcr.mode == L2CAP_FCR_ERTM_MODE) &&
          p != p_pkt_end)
        l2cu_send_peer_cmd_reject(p_lcb, L2CAP_CMD_REJ_NOT_UNDERSTOOD, last_id,
                                  0, 0);
      break;
    }

    uint8_t cmd_code, id;
    uint16_t cmd_len;
    STREAM_TO_UINT8(cmd_code, p);
    STREAM_TO_UINT8(id, p);
    STREAM_TO_UINT16(cmd_len, p);

    last_id = id;
    first_cmd = false;

    if (cmd_len > BT_SMALL_BUFFER_SIZE) {
      LOG_WARN("Command size %u exceeds limit %d", cmd_len,
               BT_SMALL_BUFFER_SIZE);
      l2cu_send_peer_cmd_reject(p_lcb, L2CAP_CMD_REJ_MTU_EXCEEDED, id, 0, 0);
      return;
    }

    /* Check command length does not exceed packet length */
    p_next_cmd = p + cmd_len;
    if (p_next_cmd > p_pkt_end) {
      LOG_WARN("cmd_len > pkt_len, pkt_len=%d, cmd_len=%d, code=%d", pkt_len,
               cmd_len, cmd_code);
      break;
    }

    LOG_DEBUG("cmd_code: %d, id:%d, cmd_len:%d", cmd_code, id, cmd_len);

    /* Bad L2CAP packet length, look for cmd to reject */
    if (pkt_size_rej) {
      /* If command found rejected it and we're done, otherwise keep looking */
      if (l2c_is_cmd_rejected(cmd_code, id, p_lcb)) {
        LOG_WARN("Rejected command %d due to bad packet length", cmd_code);
        return;
      } else {
        LOG_WARN("No need to reject command %d for bad packet len", cmd_code);
        continue; /* Look for next cmd/response in current packet */
      }
    }

    switch (cmd_code) {
      case L2CAP_CMD_REJECT:
        uint16_t rej_reason;
        if (p + 2 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_REJECT");
          return;
        }
        STREAM_TO_UINT16(rej_reason, p);
        if (rej_reason == L2CAP_CMD_REJ_MTU_EXCEEDED) {
          uint16_t rej_mtu;
          if (p + 2 > p_next_cmd) {
            LOG_WARN("Not enough data for L2CAP_CMD_REJ_MTU_EXCEEDED");
            return;
          }
          STREAM_TO_UINT16(rej_mtu, p);
          /* What to do with the MTU reject ? We have negotiated an MTU. For now
           * we will ignore it and let a higher protocol timeout take care of it
           */
          LOG_WARN("MTU rej Handle: %d MTU: %d", p_lcb->Handle(), rej_mtu);
        }
        if (rej_reason == L2CAP_CMD_REJ_INVALID_CID) {
          uint16_t lcid, rcid;
          if (p + 4 > p_next_cmd) {
            LOG_WARN("Not enough data for L2CAP_CMD_REJ_INVALID_CID");
            return;
          }
          STREAM_TO_UINT16(rcid, p);
          STREAM_TO_UINT16(lcid, p);

          LOG_WARN("Rejected due to invalid CID, LCID: 0x%04x RCID: 0x%04x",
                   lcid, rcid);

          /* Remote CID invalid. Treat as a disconnect */
          tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
          if ((p_ccb != NULL) && (p_ccb->remote_cid == rcid)) {
            /* Fake link disconnect - no reply is generated */
            LOG_WARN("Remote CID is invalid, treat as disconnected");
            l2c_csm_execute(p_ccb, L2CEVT_LP_DISCONNECT_IND, NULL);
          }
        }

        /* SonyEricsson Info request Bug workaround (Continue connection) */
        else if (rej_reason == L2CAP_CMD_REJ_NOT_UNDERSTOOD &&
                 p_lcb->w4_info_rsp) {
          alarm_cancel(p_lcb->info_resp_timer);

          p_lcb->w4_info_rsp = false;
          tL2C_CONN_INFO ci;
          ci.status = HCI_SUCCESS;
          ci.bd_addr = p_lcb->remote_bd_addr;

          /* For all channels, send the event through their FSMs */
          for (tL2C_CCB* p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb;
               p_ccb = p_ccb->p_next_ccb) {
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_INFO_RSP, &ci);
          }
        }
        break;

      case L2CAP_CMD_CONN_REQ: {
        uint16_t rcid;
        if (p + 4 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_CONN_REQ");
          return;
        }
        STREAM_TO_UINT16(con_info.psm, p);
        STREAM_TO_UINT16(rcid, p);
        p_rcb = l2cu_find_rcb_by_psm(con_info.psm);
        if (!p_rcb) {
          LOG_WARN("Rcvd conn req for unknown PSM: %d", con_info.psm);
          l2cu_reject_connection(p_lcb, rcid, id, L2CAP_CONN_NO_PSM);
          break;
        } else {
          if (!p_rcb->api.pL2CA_ConnectInd_Cb) {
            LOG_WARN("Rcvd conn req for outgoing-only connection PSM: %d",
                     con_info.psm);
            l2cu_reject_connection(p_lcb, rcid, id, L2CAP_CONN_NO_PSM);
            break;
          }
        }
        tL2C_CCB* p_ccb = l2cu_allocate_ccb(p_lcb, 0);
        if (p_ccb == nullptr) {
          LOG_ERROR("Unable to allocate CCB");
          l2cu_reject_connection(p_lcb, rcid, id, L2CAP_CONN_NO_RESOURCES);
          break;
        }
        p_ccb->remote_id = id;
        p_ccb->p_rcb = p_rcb;
        p_ccb->remote_cid = rcid;
        p_ccb->connection_initiator = L2CAP_INITIATOR_REMOTE;

        if (p_rcb->psm == BT_PSM_RFCOMM) {
          bluetooth::shim::GetSnoopLogger()->AddRfcommL2capChannel(
              p_lcb->Handle(), p_ccb->local_cid, p_ccb->remote_cid);
        } else if (p_rcb->log_packets) {
          bluetooth::shim::GetSnoopLogger()->AcceptlistL2capChannel(
              p_lcb->Handle(), p_ccb->local_cid, p_ccb->remote_cid);
        }

        l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONNECT_REQ, &con_info);
        break;
      }

      case L2CAP_CMD_CONN_RSP: {
        uint16_t lcid;
        if (p + 8 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_CONN_REQ");
          return;
        }
        STREAM_TO_UINT16(con_info.remote_cid, p);
        STREAM_TO_UINT16(lcid, p);
        STREAM_TO_UINT16(con_info.l2cap_result, p);
        STREAM_TO_UINT16(con_info.l2cap_status, p);

        tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
        if (!p_ccb) {
          LOG_WARN("no CCB for conn rsp, LCID: %d RCID: %d", lcid,
                   con_info.remote_cid);
          break;
        }
        if (p_ccb->local_id != id) {
          LOG_WARN("con rsp - bad ID. Exp: %d Got: %d", p_ccb->local_id, id);
          break;
        }

        if (con_info.l2cap_result == L2CAP_CONN_OK) {
          l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONNECT_RSP, &con_info);
        } else if (con_info.l2cap_result == L2CAP_CONN_PENDING) {
          l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONNECT_RSP_PND, &con_info);
        } else {
          l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONNECT_RSP_NEG, &con_info);

          p_rcb = p_ccb->p_rcb;
          if (p_rcb->psm == BT_PSM_RFCOMM) {
            bluetooth::shim::GetSnoopLogger()->AddRfcommL2capChannel(
                p_lcb->Handle(), p_ccb->local_cid, p_ccb->remote_cid);
          } else if (p_rcb->log_packets) {
            bluetooth::shim::GetSnoopLogger()->AcceptlistL2capChannel(
                p_lcb->Handle(), p_ccb->local_cid, p_ccb->remote_cid);
          }
        }

        break;
      }

      case L2CAP_CMD_CONFIG_REQ: {
        uint8_t* p_cfg_end = p + cmd_len;
        bool cfg_rej = false;
        uint16_t cfg_rej_len = 0;

        uint16_t lcid;
        if (p + 4 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_CONFIG_REQ");
          return;
        }
        STREAM_TO_UINT16(lcid, p);
        STREAM_TO_UINT16(cfg_info.flags, p);

        uint8_t* p_cfg_start = p;

        cfg_info.flush_to_present = cfg_info.mtu_present =
            cfg_info.qos_present = cfg_info.fcr_present = cfg_info.fcs_present =
                false;

        while (p < p_cfg_end) {
          uint8_t cfg_code, cfg_len;
          if (p + 2 > p_next_cmd) {
            LOG_WARN("Not enough data for L2CAP_CMD_CONFIG_REQ sub_event");
            return;
          }
          STREAM_TO_UINT8(cfg_code, p);
          STREAM_TO_UINT8(cfg_len, p);

          switch (cfg_code & 0x7F) {
            case L2CAP_CFG_TYPE_MTU:
              cfg_info.mtu_present = true;
              if (cfg_len != 2) {
                return;
              }
              if (p + cfg_len > p_next_cmd) {
                return;
              }
              STREAM_TO_UINT16(cfg_info.mtu, p);
              break;

            case L2CAP_CFG_TYPE_FLUSH_TOUT:
              cfg_info.flush_to_present = true;
              if (cfg_len != 2) {
                return;
              }
              if (p + cfg_len > p_next_cmd) {
                return;
              }
              STREAM_TO_UINT16(cfg_info.flush_to, p);
              break;

            case L2CAP_CFG_TYPE_QOS:
              cfg_info.qos_present = true;
              if (cfg_len != 2 + 5 * 4) {
                return;
              }
              if (p + cfg_len > p_next_cmd) {
                return;
              }
              STREAM_TO_UINT8(cfg_info.qos.qos_flags, p);
              STREAM_TO_UINT8(cfg_info.qos.service_type, p);
              STREAM_TO_UINT32(cfg_info.qos.token_rate, p);
              STREAM_TO_UINT32(cfg_info.qos.token_bucket_size, p);
              STREAM_TO_UINT32(cfg_info.qos.peak_bandwidth, p);
              STREAM_TO_UINT32(cfg_info.qos.latency, p);
              STREAM_TO_UINT32(cfg_info.qos.delay_variation, p);
              break;

            case L2CAP_CFG_TYPE_FCR:
              cfg_info.fcr_present = true;
              if (cfg_len != 3 + 3 * 2) {
                return;
              }
              if (p + cfg_len > p_next_cmd) {
                return;
              }
              STREAM_TO_UINT8(cfg_info.fcr.mode, p);
              STREAM_TO_UINT8(cfg_info.fcr.tx_win_sz, p);
              STREAM_TO_UINT8(cfg_info.fcr.max_transmit, p);
              STREAM_TO_UINT16(cfg_info.fcr.rtrans_tout, p);
              STREAM_TO_UINT16(cfg_info.fcr.mon_tout, p);
              STREAM_TO_UINT16(cfg_info.fcr.mps, p);
              break;

            case L2CAP_CFG_TYPE_FCS:
              cfg_info.fcs_present = true;
              if (cfg_len != 1) {
                return;
              }
              if (p + cfg_len > p_next_cmd) {
                return;
              }
              STREAM_TO_UINT8(cfg_info.fcs, p);
              break;

            case L2CAP_CFG_TYPE_EXT_FLOW:
              cfg_info.ext_flow_spec_present = true;
              if (cfg_len != 2 + 2 + 3 * 4) {
                return;
              }
              if (p + cfg_len > p_next_cmd) {
                return;
              }
              STREAM_TO_UINT8(cfg_info.ext_flow_spec.id, p);
              STREAM_TO_UINT8(cfg_info.ext_flow_spec.stype, p);
              STREAM_TO_UINT16(cfg_info.ext_flow_spec.max_sdu_size, p);
              STREAM_TO_UINT32(cfg_info.ext_flow_spec.sdu_inter_time, p);
              STREAM_TO_UINT32(cfg_info.ext_flow_spec.access_latency, p);
              STREAM_TO_UINT32(cfg_info.ext_flow_spec.flush_timeout, p);
              break;

            default:
              /* sanity check option length */
              if ((cfg_len + L2CAP_CFG_OPTION_OVERHEAD) <= cmd_len) {
                if (p + cfg_len > p_next_cmd) return;
                p += cfg_len;
                if ((cfg_code & 0x80) == 0) {
                  cfg_rej_len += cfg_len + L2CAP_CFG_OPTION_OVERHEAD;
                  cfg_rej = true;
                }
              }
              /* bad length; force loop exit */
              else {
                p = p_cfg_end;
                cfg_rej = true;
              }
              break;
          }
        }

        tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
        if (p_ccb) {
          p_ccb->remote_id = id;
          if (cfg_rej) {
            l2cu_send_peer_config_rej(
                p_ccb, p_cfg_start, (uint16_t)(cmd_len - L2CAP_CONFIG_REQ_LEN),
                cfg_rej_len);
          } else {
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONFIG_REQ, &cfg_info);
          }
        } else {
          /* updated spec says send command reject on invalid cid */
          l2cu_send_peer_cmd_reject(p_lcb, L2CAP_CMD_REJ_INVALID_CID, id, 0, 0);
        }
        break;
      }

      case L2CAP_CMD_CONFIG_RSP: {
        uint8_t* p_cfg_end = p + cmd_len;
        uint16_t lcid;
        if (p + 6 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_CONFIG_RSP");
          return;
        }
        STREAM_TO_UINT16(lcid, p);
        STREAM_TO_UINT16(cfg_info.flags, p);
        STREAM_TO_UINT16(cfg_info.result, p);

        cfg_info.flush_to_present = cfg_info.mtu_present =
            cfg_info.qos_present = cfg_info.fcr_present = cfg_info.fcs_present =
                false;

        while (p < p_cfg_end) {
          uint8_t cfg_code, cfg_len;
          if (p + 2 > p_next_cmd) {
            LOG_WARN("Not enough data for L2CAP_CMD_CONFIG_RSP sub_event");
            return;
          }
          STREAM_TO_UINT8(cfg_code, p);
          STREAM_TO_UINT8(cfg_len, p);

          switch (cfg_code & 0x7F) {
            case L2CAP_CFG_TYPE_MTU:
              cfg_info.mtu_present = true;
              if (p + 2 > p_next_cmd) {
                LOG_WARN("Not enough data for L2CAP_CFG_TYPE_MTU");
                return;
              }
              STREAM_TO_UINT16(cfg_info.mtu, p);
              break;

            case L2CAP_CFG_TYPE_FLUSH_TOUT:
              cfg_info.flush_to_present = true;
              if (p + 2 > p_next_cmd) {
                LOG_WARN("Not enough data for L2CAP_CFG_TYPE_FLUSH_TOUT");
                return;
              }
              STREAM_TO_UINT16(cfg_info.flush_to, p);
              break;

            case L2CAP_CFG_TYPE_QOS:
              cfg_info.qos_present = true;
              if (p + 2 + 5 * 4 > p_next_cmd) {
                LOG_WARN("Not enough data for L2CAP_CFG_TYPE_QOS");
                return;
              }
              STREAM_TO_UINT8(cfg_info.qos.qos_flags, p);
              STREAM_TO_UINT8(cfg_info.qos.service_type, p);
              STREAM_TO_UINT32(cfg_info.qos.token_rate, p);
              STREAM_TO_UINT32(cfg_info.qos.token_bucket_size, p);
              STREAM_TO_UINT32(cfg_info.qos.peak_bandwidth, p);
              STREAM_TO_UINT32(cfg_info.qos.latency, p);
              STREAM_TO_UINT32(cfg_info.qos.delay_variation, p);
              break;

            case L2CAP_CFG_TYPE_FCR:
              cfg_info.fcr_present = true;
              if (p + 3 + 3 * 2 > p_next_cmd) {
                LOG_WARN("Not enough data for L2CAP_CFG_TYPE_FCR");
                return;
              }
              STREAM_TO_UINT8(cfg_info.fcr.mode, p);
              STREAM_TO_UINT8(cfg_info.fcr.tx_win_sz, p);
              STREAM_TO_UINT8(cfg_info.fcr.max_transmit, p);
              STREAM_TO_UINT16(cfg_info.fcr.rtrans_tout, p);
              STREAM_TO_UINT16(cfg_info.fcr.mon_tout, p);
              STREAM_TO_UINT16(cfg_info.fcr.mps, p);
              break;

            case L2CAP_CFG_TYPE_FCS:
              cfg_info.fcs_present = true;
              if (p + 1 > p_next_cmd) {
                LOG_WARN("Not enough data for L2CAP_CFG_TYPE_FCS");
                return;
              }
              STREAM_TO_UINT8(cfg_info.fcs, p);
              break;

            case L2CAP_CFG_TYPE_EXT_FLOW:
              cfg_info.ext_flow_spec_present = true;
              if (p + 2 + 2 + 3 * 4 > p_next_cmd) {
                LOG_WARN("Not enough data for L2CAP_CFG_TYPE_EXT_FLOW");
                return;
              }
              STREAM_TO_UINT8(cfg_info.ext_flow_spec.id, p);
              STREAM_TO_UINT8(cfg_info.ext_flow_spec.stype, p);
              STREAM_TO_UINT16(cfg_info.ext_flow_spec.max_sdu_size, p);
              STREAM_TO_UINT32(cfg_info.ext_flow_spec.sdu_inter_time, p);
              STREAM_TO_UINT32(cfg_info.ext_flow_spec.access_latency, p);
              STREAM_TO_UINT32(cfg_info.ext_flow_spec.flush_timeout, p);
              break;
          }
        }

        tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
        if (p_ccb) {
          if (p_ccb->local_id != id) {
            LOG_WARN("cfg rsp - bad ID. Exp: %d Got: %d", p_ccb->local_id, id);
            break;
          }
          if (cfg_info.result == L2CAP_CFG_OK) {
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONFIG_RSP, &cfg_info);
          } else {
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_CONFIG_RSP_NEG, &cfg_info);
          }
        } else {
          LOG_WARN("Rcvd cfg rsp for unknown CID: 0x%04x", lcid);
        }
        break;
      }

      case L2CAP_CMD_DISC_REQ: {
        uint16_t lcid, rcid;
        if (p + 4 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_DISC_REQ");
          return;
        }
        STREAM_TO_UINT16(lcid, p);
        STREAM_TO_UINT16(rcid, p);

        tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
        if (p_ccb) {
          if (p_ccb->remote_cid == rcid) {
            p_ccb->remote_id = id;
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_DISCONNECT_REQ, &con_info);
          }
        } else
          l2cu_send_peer_disc_rsp(p_lcb, id, lcid, rcid);

        break;
      }

      case L2CAP_CMD_DISC_RSP: {
        uint16_t lcid, rcid;
        if (p + 4 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_DISC_RSP");
          return;
        }
        STREAM_TO_UINT16(rcid, p);
        STREAM_TO_UINT16(lcid, p);

        tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(p_lcb, lcid);
        if (p_ccb) {
          if ((p_ccb->remote_cid == rcid) && (p_ccb->local_id == id)) {
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_DISCONNECT_RSP, &con_info);
          }
        }
        break;
      }

      case L2CAP_CMD_ECHO_REQ:
        l2cu_send_peer_echo_rsp(p_lcb, id, p, cmd_len);
        break;

      case L2CAP_CMD_INFO_REQ: {
        uint16_t info_type;
        if (p + 2 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_INFO_REQ");
          return;
        }
        STREAM_TO_UINT16(info_type, p);
        l2cu_send_peer_info_rsp(p_lcb, id, info_type);
        break;
      }

      case L2CAP_CMD_INFO_RSP:
        /* Stop the link connect timer if sent before L2CAP connection is up */
        if (p_lcb->w4_info_rsp) {
          alarm_cancel(p_lcb->info_resp_timer);
          p_lcb->w4_info_rsp = false;
        }

        uint16_t info_type, result;
        if (p + 4 > p_next_cmd) {
          LOG_WARN("Not enough data for L2CAP_CMD_INFO_RSP");
          return;
        }
        STREAM_TO_UINT16(info_type, p);
        STREAM_TO_UINT16(result, p);

        if ((info_type == L2CAP_EXTENDED_FEATURES_INFO_TYPE) &&
            (result == L2CAP_INFO_RESP_RESULT_SUCCESS)) {
          if (p + 4 > p_next_cmd) {
            LOG_WARN("Not enough data for L2CAP_CMD_INFO_RSP sub_event");
            return;
          }
          STREAM_TO_UINT32(p_lcb->peer_ext_fea, p);

          if (p_lcb->peer_ext_fea & L2CAP_EXTFEA_FIXED_CHNLS) {
            l2cu_send_peer_info_req(p_lcb, L2CAP_FIXED_CHANNELS_INFO_TYPE);
            break;
          } else {
            l2cu_process_fixed_chnl_resp(p_lcb);
          }
        }

        if (info_type == L2CAP_FIXED_CHANNELS_INFO_TYPE) {
          if (result == L2CAP_INFO_RESP_RESULT_SUCCESS) {
            if (p + L2CAP_FIXED_CHNL_ARRAY_SIZE > p_next_cmd) {
              return;
            }
            memcpy(p_lcb->peer_chnl_mask, p, L2CAP_FIXED_CHNL_ARRAY_SIZE);
          }

          l2cu_process_fixed_chnl_resp(p_lcb);
        }
        {
          tL2C_CONN_INFO ci;
          ci.status = HCI_SUCCESS;
          ci.bd_addr = p_lcb->remote_bd_addr;
          for (tL2C_CCB* p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb;
               p_ccb = p_ccb->p_next_ccb) {
            l2c_csm_execute(p_ccb, L2CEVT_L2CAP_INFO_RSP, &ci);
          }
        }
        break;

      default:
        LOG_WARN("Bad cmd code: %d", cmd_code);
        l2cu_send_peer_cmd_reject(p_lcb, L2CAP_CMD_REJ_NOT_UNDERSTOOD, id, 0,
                                  0);
        return;
    }
  }
}

/*******************************************************************************
 *
 * Function         l2c_init
 *
 * Description      This function is called once at startup to initialize
 *                  all the L2CAP structures
 *
 * Returns          void
 *
 ******************************************************************************/
void l2c_init(void) {
  if (bluetooth::shim::is_gd_l2cap_enabled()) {
    // L2CAP init should be handled by GD stack manager
    return;
  }

  int16_t xx;

  memset(&l2cb, 0, sizeof(tL2C_CB));

  /* the LE PSM is increased by 1 before being used */
  l2cb.le_dyn_psm = LE_DYNAMIC_PSM_START - 1;

  /* Put all the channel control blocks on the free queue */
  for (xx = 0; xx < MAX_L2CAP_CHANNELS - 1; xx++) {
    l2cb.ccb_pool[xx].p_next_ccb = &l2cb.ccb_pool[xx + 1];
  }

  /* it will be set to L2CAP_PKT_START_NON_FLUSHABLE if controller supports */
  l2cb.non_flushable_pbf = L2CAP_PKT_START << L2CAP_PKT_TYPE_SHIFT;

  l2cb.p_free_ccb_first = &l2cb.ccb_pool[0];
  l2cb.p_free_ccb_last = &l2cb.ccb_pool[MAX_L2CAP_CHANNELS - 1];

  /* Set the default idle timeout */
  l2cb.idle_timeout = L2CAP_LINK_INACTIVITY_TOUT;

#if defined(L2CAP_INITIAL_TRACE_LEVEL)
  l2cb.l2cap_trace_level = L2CAP_INITIAL_TRACE_LEVEL;
#else
  l2cb.l2cap_trace_level = BT_TRACE_LEVEL_NONE; /* No traces */
#endif

#if (L2CAP_CONFORMANCE_TESTING == TRUE)
  /* Conformance testing needs a dynamic response */
  l2cb.test_info_resp = L2CAP_EXTFEA_SUPPORTED_MASK;
#endif

  /* Number of ACL buffers to use for high priority channel */

  l2cb.l2c_ble_fixed_chnls_mask = L2CAP_FIXED_CHNL_ATT_BIT |
                                  L2CAP_FIXED_CHNL_BLE_SIG_BIT |
                                  L2CAP_FIXED_CHNL_SMP_BIT;
}

void l2c_free(void) {
  if (bluetooth::shim::is_gd_l2cap_enabled()) {
    // L2CAP cleanup should be handled by GD stack manager
    return;
  }
}

void l2c_ccb_timer_timeout(void* data) {
  tL2C_CCB* p_ccb = (tL2C_CCB*)data;

  l2c_csm_execute(p_ccb, L2CEVT_TIMEOUT, NULL);
}

void l2c_fcrb_ack_timer_timeout(void* data) {
  tL2C_CCB* p_ccb = (tL2C_CCB*)data;

  l2c_csm_execute(p_ccb, L2CEVT_ACK_TIMEOUT, NULL);
}

void l2c_lcb_timer_timeout(void* data) {
  tL2C_LCB* p_lcb = (tL2C_LCB*)data;

  l2c_link_timeout(p_lcb);
}

/*******************************************************************************
 *
 * Function         l2c_data_write
 *
 * Description      API functions call this function to write data.
 *
 * Returns          L2CAP_DW_SUCCESS, if data accepted, else false
 *                  L2CAP_DW_CONGESTED, if data accepted and the channel is
 *                                      congested
 *                  L2CAP_DW_FAILED, if error
 *
 ******************************************************************************/
uint8_t l2c_data_write(uint16_t cid, BT_HDR* p_data, uint16_t flags) {
  /* Find the channel control block. We don't know the link it is on. */
  tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(NULL, cid);
  if (!p_ccb) {
    L2CAP_TRACE_WARNING("L2CAP - no CCB for L2CA_DataWrite, CID: %d", cid);
    osi_free(p_data);
    return (L2CAP_DW_FAILED);
  }

  /* Sending message bigger than mtu size of peer is a violation of protocol */
  uint16_t mtu;

  if (p_ccb->p_lcb->transport == BT_TRANSPORT_LE)
    mtu = p_ccb->peer_conn_cfg.mtu;
  else
    mtu = p_ccb->peer_cfg.mtu;

  if (p_data->len > mtu) {
    L2CAP_TRACE_WARNING(
        "L2CAP - CID: 0x%04x  cannot send message bigger than peer's mtu size: "
        "len=%u mtu=%u",
        cid, p_data->len, mtu);
    osi_free(p_data);
    return (L2CAP_DW_FAILED);
  }

  /* channel based, packet based flushable or non-flushable */
  p_data->layer_specific = flags;

  /* If already congested, do not accept any more packets */
  if (p_ccb->cong_sent) {
    L2CAP_TRACE_ERROR(
        "L2CAP - CID: 0x%04x cannot send, already congested  "
        "xmit_hold_q.count: %u  buff_quota: %u",
        p_ccb->local_cid, fixed_queue_length(p_ccb->xmit_hold_q),
        p_ccb->buff_quota);

    osi_free(p_data);
    return (L2CAP_DW_FAILED);
  }

  l2c_csm_execute(p_ccb, L2CEVT_L2CA_DATA_WRITE, p_data);

  if (p_ccb->cong_sent) return (L2CAP_DW_CONGESTED);

  return (L2CAP_DW_SUCCESS);
}