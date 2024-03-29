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
 *  this file contains the PORT API definitions
 *
 ******************************************************************************/
#ifndef PORT_API_H
#define PORT_API_H

#include <cstdint>

#include "internal_include/bt_target.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants and Types
 ****************************************************************************/

/*
 * Define port settings structure send from the application in the
 * set settings request, or to the application in the set settings indication.
*/
typedef struct {
#define PORT_BAUD_RATE_9600 0x03

  uint8_t baud_rate;

#define PORT_8_BITS 0x03

  uint8_t byte_size;

#define PORT_ONESTOPBIT 0x00
  uint8_t stop_bits;

#define PORT_PARITY_NO 0x00
  uint8_t parity;

#define PORT_ODD_PARITY 0x00

  uint8_t parity_type;

#define PORT_FC_OFF 0x00
#define PORT_FC_CTS_ON_INPUT 0x04
#define PORT_FC_CTS_ON_OUTPUT 0x08

  uint8_t fc_type;

  uint8_t rx_char1;

#define PORT_XON_DC1 0x11
  uint8_t xon_char;

#define PORT_XOFF_DC3 0x13
  uint8_t xoff_char;

} tPORT_STATE;

/*
 * Define the callback function prototypes.  Parameters are specific
 * to each event and are described bellow
*/
typedef int(tPORT_DATA_CALLBACK)(uint16_t port_handle, void* p_data,
                                 uint16_t len);

#define DATA_CO_CALLBACK_TYPE_INCOMING 1
#define DATA_CO_CALLBACK_TYPE_OUTGOING_SIZE 2
#define DATA_CO_CALLBACK_TYPE_OUTGOING 3
typedef int(tPORT_DATA_CO_CALLBACK)(uint16_t port_handle, uint8_t* p_buf,
                                    uint16_t len, int type);

typedef void(tPORT_CALLBACK)(uint32_t code, uint16_t port_handle);

/*
 * Define events that registered application can receive in the callback
*/

#define PORT_EV_RXCHAR 0x00000001  /* Any Character received */
#define PORT_EV_RXFLAG 0x00000002  /* Received certain character */
#define PORT_EV_TXEMPTY 0x00000004 /* Transmitt Queue Empty */
#define PORT_EV_CTS 0x00000008     /* CTS changed state */
#define PORT_EV_DSR 0x00000010     /* DSR changed state */
#define PORT_EV_RLSD 0x00000020    /* RLSD changed state */
#define PORT_EV_BREAK 0x00000040   /* BREAK received */
#define PORT_EV_ERR 0x00000080     /* Line status error occurred */
#define PORT_EV_RING 0x00000100    /* Ring signal detected */
#define PORT_EV_CTSS 0x00000400    /* CTS state */
#define PORT_EV_DSRS 0x00000800    /* DSR state */
#define PORT_EV_RLSDS 0x00001000   /* RLSD state */
#define PORT_EV_OVERRUN 0x00002000 /* receiver buffer overrun */
#define PORT_EV_TXCHAR 0x00004000  /* Any character transmitted */

/* RFCOMM connection established */
#define PORT_EV_CONNECTED 0x00000200
/* Unable to establish connection  or disconnected */
#define PORT_EV_CONNECT_ERR 0x00008000
/* data flow enabled flag changed by remote */
#define PORT_EV_FC 0x00010000
/* data flow enable status true = enabled */
#define PORT_EV_FCS 0x00020000

/*
 * Define port result codes
*/
#define PORT_SUCCESS 0

#define PORT_ERR_BASE 0

#define PORT_UNKNOWN_ERROR (PORT_ERR_BASE + 1)
#define PORT_ALREADY_OPENED (PORT_ERR_BASE + 2)
#define PORT_CMD_PENDING (PORT_ERR_BASE + 3)
#define PORT_APP_NOT_REGISTERED (PORT_ERR_BASE + 4)
#define PORT_NO_MEM (PORT_ERR_BASE + 5)
#define PORT_NO_RESOURCES (PORT_ERR_BASE + 6)
#define PORT_BAD_BD_ADDR (PORT_ERR_BASE + 7)
#define PORT_BAD_HANDLE (PORT_ERR_BASE + 9)
#define PORT_NOT_OPENED (PORT_ERR_BASE + 10)
#define PORT_LINE_ERR (PORT_ERR_BASE + 11)
#define PORT_START_FAILED (PORT_ERR_BASE + 12)
#define PORT_PAR_NEG_FAILED (PORT_ERR_BASE + 13)
#define PORT_PORT_NEG_FAILED (PORT_ERR_BASE + 14)
#define PORT_SEC_FAILED (PORT_ERR_BASE + 15)
#define PORT_PEER_CONNECTION_FAILED (PORT_ERR_BASE + 16)
#define PORT_PEER_FAILED (PORT_ERR_BASE + 17)
#define PORT_PEER_TIMEOUT (PORT_ERR_BASE + 18)
#define PORT_CLOSED (PORT_ERR_BASE + 19)
#define PORT_TX_FULL (PORT_ERR_BASE + 20)
#define PORT_LOCAL_CLOSED (PORT_ERR_BASE + 21)
#define PORT_LOCAL_TIMEOUT (PORT_ERR_BASE + 22)
#define PORT_TX_QUEUE_DISABLED (PORT_ERR_BASE + 23)
#define PORT_PAGE_TIMEOUT (PORT_ERR_BASE + 24)
#define PORT_INVALID_SCN (PORT_ERR_BASE + 25)

#define PORT_ERR_MAX (PORT_ERR_BASE + 26)

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/

/*******************************************************************************
 *
 * Function         RFCOMM_CreateConnection
 *
 * Description      RFCOMM_CreateConnection is used from the application to
 *                  establish a serial port connection to the peer device,
 *                  or allow RFCOMM to accept a connection from the peer
 *                  application.
 *
 * Parameters:      scn          - Service Channel Number as registered with
 *                                 the SDP (server) or obtained using SDP from
 *                                 the peer device (client).
 *                  is_server    - true if requesting application is a server
 *                  mtu          - Maximum frame size the application can accept
 *                  bd_addr      - address of the peer (client)
 *                  mask         - specifies events to be enabled.  A value
 *                                 of zero disables all events.
 *                  p_handle     - OUT pointer to the handle.
 *                  p_mgmt_cb    - pointer to callback function to receive
 *                                 connection up/down events.
 * Notes:
 *
 * Server can call this function with the same scn parameter multiple times if
 * it is ready to accept multiple simulteneous connections.
 *
 * DLCI for the connection is (scn * 2 + 1) if client originates connection on
 * existing none initiator multiplexer channel.  Otherwise it is (scn * 2).
 * For the server DLCI can be changed later if client will be calling it using
 * (scn * 2 + 1) dlci.
 *
 ******************************************************************************/
int RFCOMM_CreateConnectionWithSecurity(uint16_t uuid, uint8_t scn,
                                        bool is_server, uint16_t mtu,
                                        const RawAddress& bd_addr,
                                        uint16_t* p_handle,
                                        tPORT_CALLBACK* p_mgmt_cb,
                                        uint16_t sec_mask);

/*******************************************************************************
 *
 * Function         RFCOMM_ControlReqFromBTSOCK
 *
 * Description      Send control parameters to the peer.
 *                  So far only for qualification use.
 *                  RFCOMM layer starts the control request only when it is the
 *                  client. This API allows the host to start the control
 *                  request while it works as a RFCOMM server.
 *
 * Parameters:      dlci             - the DLCI to send the MSC command
 *                  bd_addr          - bd_addr of the peer
 *                  modem_signal     - [DTR/DSR | RTS/CTS | RI | DCD]
 *                  break_signal     - 0-3 s in steps of 200 ms
 *                  discard_buffers  - 0 for do not discard, 1 for discard
 *                  break_signal_seq - ASAP or in sequence
 *                  fc               - true when the device is unable to accept
 *                                     frames
 *
 ******************************************************************************/
extern int RFCOMM_ControlReqFromBTSOCK(uint8_t dlci, const RawAddress& bd_addr,
                                       uint8_t modem_signal,
                                       uint8_t break_signal,
                                       uint8_t discard_buffers,
                                       uint8_t break_signal_seq, bool fc);

/*******************************************************************************
 *
 * Function         RFCOMM_RemoveConnection
 *
 * Description      This function is called to close the specified connection.
 *
 * Parameters:      handle     - Handle of the port returned in the Open
 *
 ******************************************************************************/
int RFCOMM_RemoveConnection(uint16_t handle);

/*******************************************************************************
 *
 * Function         RFCOMM_RemoveServer
 *
 * Description      This function is called to close the server port.
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *
 ******************************************************************************/
int RFCOMM_RemoveServer(uint16_t handle);

/*******************************************************************************
 *
 * Function         PORT_SetEventCallback
 *
 * Description      Set event callback the specified connection.
 *
 * Parameters:      handle       - Handle of the port returned in the Open
 *                  p_callback   - address of the callback function which should
 *                                 be called from the RFCOMM when an event
 *                                 specified in the mask occurs.
 *
 ******************************************************************************/
int PORT_SetEventCallback(uint16_t port_handle, tPORT_CALLBACK* p_port_cb);

/*******************************************************************************
 *
 * Function         PORT_ClearKeepHandleFlag
 *
 * Description      Called to clear the keep handle flag, which will cause
 *                  not to keep the port handle open when closed
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *
 ******************************************************************************/
int PORT_ClearKeepHandleFlag(uint16_t port_handle);

int PORT_SetDataCOCallback(uint16_t port_handle,
                           tPORT_DATA_CO_CALLBACK* p_port_cb);
/*******************************************************************************
 *
 * Function         PORT_SetEventMask
 *
 * Description      This function is called to close the specified connection.
 *
 * Parameters:      handle - Handle of the port returned in the Open
 *                  mask   - specifies events to be enabled.  A value
 *                           of zero disables all events.
 *
 ******************************************************************************/
int PORT_SetEventMask(uint16_t port_handle, uint32_t mask);

/*******************************************************************************
 *
 * Function         PORT_CheckConnection
 *
 * Description      This function returns PORT_SUCCESS if connection referenced
 *                  by handle is up and running
 *
 * Parameters:      handle     - Handle of the port returned in the Open
 *                  bd_addr    - OUT bd_addr of the peer
 *                  p_lcid     - OUT L2CAP's LCID
 *
 ******************************************************************************/
int PORT_CheckConnection(uint16_t handle, RawAddress* bd_addr,
                         uint16_t* p_lcid);

/*******************************************************************************
 *
 * Function         PORT_IsOpening
 *
 * Description      This function returns true if there is any RFCOMM connection
 *                  opening in process.
 *
 * Parameters:      true if any connection opening is found
 *                  bd_addr    - bd_addr of the peer
 *
 ******************************************************************************/
bool PORT_IsOpening(RawAddress* bd_addr);

/*******************************************************************************
 *
 * Function         PORT_SetState
 *
 * Description      This function configures connection according to the
 *                  specifications in the tPORT_STATE structure.
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *                  p_settings - Pointer to a tPORT_STATE structure containing
 *                               configuration information for the connection.
 *
 ******************************************************************************/
int PORT_SetState(uint16_t handle, tPORT_STATE* p_settings);

/*******************************************************************************
 *
 * Function         PORT_GetState
 *
 * Description      This function is called to fill tPORT_STATE structure
 *                  with the current control settings for the port
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *                  p_settings - Pointer to a tPORT_STATE structure in which
 *                               configuration information is returned.
 *
 ******************************************************************************/
int PORT_GetState(uint16_t handle, tPORT_STATE* p_settings);

/*******************************************************************************
 *
 * Function         PORT_FlowControl_MaxCredit
 *
 * Description      This function directs a specified connection to pass
 *                  flow control message to the peer device.  Enable flag passed
 *                  shows if port can accept more data. It also sends max credit
 *                  when data flow enabled
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *                  enable     - enables data flow
 *
 ******************************************************************************/
int PORT_FlowControl_MaxCredit(uint16_t handle, bool enable);

#define PORT_DTRDSR_ON 0x01
#define PORT_CTSRTS_ON 0x02
#define PORT_RING_ON 0x04
#define PORT_DCD_ON 0x08

/*
 * Define default initial local modem signals state after connection established
*/
#define PORT_OBEX_DEFAULT_SIGNAL_STATE \
  (PORT_DTRDSR_ON | PORT_CTSRTS_ON | PORT_DCD_ON)
#define PORT_SPP_DEFAULT_SIGNAL_STATE \
  (PORT_DTRDSR_ON | PORT_CTSRTS_ON | PORT_DCD_ON)
#define PORT_PPP_DEFAULT_SIGNAL_STATE \
  (PORT_DTRDSR_ON | PORT_CTSRTS_ON | PORT_DCD_ON)
#define PORT_DUN_DEFAULT_SIGNAL_STATE (PORT_DTRDSR_ON | PORT_CTSRTS_ON)

#define PORT_ERR_BREAK 0x01   /* Break condition occured on the peer device */
#define PORT_ERR_OVERRUN 0x02 /* Overrun is reported by peer device */
#define PORT_ERR_FRAME 0x04   /* Framing error reported by peer device */
#define PORT_ERR_RXOVER 0x08  /* Input queue overflow occured */
#define PORT_ERR_TXFULL 0x10  /* Output queue overflow occured */

/*******************************************************************************
 *
 * Function         PORT_ReadData
 *
 * Description      Normally application will call this function after receiving
 *                  PORT_EVT_RXCHAR event.
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *                                callback.
 *                  p_data      - Data area
 *                  max_len     - Byte count requested
 *                  p_len       - Byte count received
 *
 ******************************************************************************/
int PORT_ReadData(uint16_t handle, char* p_data, uint16_t max_len,
                  uint16_t* p_len);

/*******************************************************************************
 *
 * Function         PORT_WriteData
 *
 * Description      This function is called from the legacy application to
 *                  send data.
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *                  p_data      - Data area
 *                  max_len     - Byte count to write
 *                  p_len       - Bytes written
 *
 ******************************************************************************/
int PORT_WriteData(uint16_t handle, const char* p_data, uint16_t max_len,
                   uint16_t* p_len);

/*******************************************************************************
 *
 * Function         PORT_WriteDataCO
 *
 * Description      Normally not GKI aware application will call this function
 *                  to send data to the port by callout functions.
 *
 * Parameters:      handle     - Handle returned in the RFCOMM_CreateConnection
 *
 ******************************************************************************/
int PORT_WriteDataCO(uint16_t handle, int* p_len);

/*******************************************************************************
 *
 * Function         RFCOMM_Init
 *
 * Description      This function is called to initialize RFCOMM layer
 *
 ******************************************************************************/
void RFCOMM_Init(void);

/*******************************************************************************
 *
 * Function         PORT_GetResultString
 *
 * Description      This function returns the human-readable string for a given
 *                  result code.
 *
 * Returns          a pointer to the human-readable string for the given
 *                  result. Note that the string returned must not be freed.
 *
 ******************************************************************************/
const char* PORT_GetResultString(const uint8_t result_code);

/*******************************************************************************
 *
 * Function         PORT_GetSecurityMask
 *
 * Description      This function returns the security bitmask for a port.
 *
 * Returns          the security bitmask.
 *
 ******************************************************************************/
int PORT_GetSecurityMask(uint16_t handle, uint16_t* sec_mask);

#endif /* PORT_API_H */
