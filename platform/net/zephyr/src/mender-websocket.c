/**
 * @file      mender-websocket.c
 * @brief     Mender websocket interface for Zephyr platform
 *
 * Copyright joelguittet and mender-mcu-client contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <version.h>
#include <zephyr/kernel.h>
#include <zephyr/net/websocket.h>
#include "mender-log.h"
#include "mender-net.h"
#include "mender-websocket.h"

/**
 * @brief Check size of the heap memory pool
 */
#if (0 == CONFIG_HEAP_MEM_POOL_SIZE)
#error "CONFIG_HEAP_MEM_POOL_SIZE is not defined"
#endif

/**
 * @brief Default websocket thread stack size (kB)
 */
#ifndef CONFIG_MENDER_WEBSOCKET_THREAD_STACK_SIZE
#define CONFIG_MENDER_WEBSOCKET_THREAD_STACK_SIZE (4)
#endif

/**
 * @brief Default websocket thread priority
 */
#ifndef CONFIG_MENDER_WEBSOCKET_THREAD_PRIORITY
#define CONFIG_MENDER_WEBSOCKET_THREAD_PRIORITY (5)
#endif /* CONFIG_MENDER_WEBSOCKET_THREAD_PRIORITY */

/**
 * @brief Websocket connect timeout (milliseconds)
 */
#ifndef CONFIG_MENDER_WEBSOCKET_CONNECT_TIMEOUT
#define CONFIG_MENDER_WEBSOCKET_CONNECT_TIMEOUT (3000)
#endif /* CONFIG_MENDER_WEBSOCKET_CONNECT_TIMEOUT */

/**
 * @brief Websocket request timeout (milliseconds)
 */
#ifndef CONFIG_MENDER_WEBSOCKET_REQUEST_TIMEOUT
#define CONFIG_MENDER_WEBSOCKET_REQUEST_TIMEOUT (3000)
#endif /* CONFIG_MENDER_WEBSOCKET_REQUEST_TIMEOUT */

/**
 * @brief WebSocket User-Agent
 */
#define MENDER_HEADER_WEBSOCKET_USER_AGENT "User-Agent: Mender/" MENDER_CLIENT_VERSION " MCU Zephyr(websocket)/" KERNEL_VERSION_STRING "\r\n"

/**
 * @brief Websocket receive buffer length
 */
#define MENDER_WEBSOCKET_RECV_BUF_LENGTH (3 * 512)

/**
 * @brief Websocket handle
 */
typedef struct {
    int             sock;          /**< Socket */
    int             client;        /**< Websocket client handle */
    struct k_thread thread_handle; /**< Websocket thread handle */
    bool            abort;         /**< Flag used to indicate connection should be terminated */
    mender_err_t (*callback)(mender_websocket_client_event_t,
                             void *,
                             size_t,
                             void *); /**< Callback function to be invoked to perform the treatment of the events and data from the websocket */
    void *params;                     /**< Callback function parameters */
} mender_websocket_handle_t;

/**
 * @brief Mender websocket configuration
 */
static mender_websocket_config_t mender_websocket_config;

/**
 * @brief Mender websocket thread stack
 */
K_THREAD_STACK_DEFINE(mender_websocket_thread_stack, CONFIG_MENDER_WEBSOCKET_THREAD_STACK_SIZE * 1024);

/**
 * @brief Thread used to perform reception of data
 * @param p1 Websocket handle
 * @param p2 Not used
 * @param p3 Not used
 */
static void mender_websocket_thread(void *p1, void *p2, void *p3);

mender_err_t
mender_websocket_init(mender_websocket_config_t *config) {

    assert(NULL != config);
    assert(NULL != config->host);

    /* Save configuration */
    memcpy(&mender_websocket_config, config, sizeof(mender_websocket_config_t));

    return MENDER_OK;
}

mender_err_t
mender_websocket_connect(
    char *jwt, char *path, mender_err_t (*callback)(mender_websocket_client_event_t, void *, size_t, void *), void *params, void **handle) {

    assert(NULL != path);
    assert(NULL != handle);
    mender_err_t             ret                = MENDER_FAIL;
    struct websocket_request request            = { 0 };
    const char              *header_fields[3]   = { NULL };
    size_t                   header_fields_size = sizeof(header_fields) / sizeof(header_fields[0]);
    char                    *host               = NULL;
    char                    *port               = NULL;
    char                    *url                = NULL;

    /* Headers to be added to the request */
    char *auth_header = NULL;

    /* Allocate a new handle */
    if (NULL == (*handle = calloc(1, sizeof(mender_websocket_handle_t)))) {
        mender_log_error("Unable to allocate memory");
        goto FAIL;
    }

    ((mender_websocket_handle_t *)*handle)->sock = -1;

    /* Save callback and params */
    ((mender_websocket_handle_t *)*handle)->callback = callback;
    ((mender_websocket_handle_t *)*handle)->params   = params;

    /* Retrieve host, port and url */
    if (MENDER_OK != mender_net_get_host_port_url(path, mender_websocket_config.host, &host, &port, &url)) {
        mender_log_error("Unable to retrieve host/port/url");
        goto FAIL;
    }

    /* Configuration of the client */
    request.url  = url;
    request.host = host;
    if (NULL == (request.tmp_buf = (uint8_t *)malloc(MENDER_WEBSOCKET_RECV_BUF_LENGTH))) {
        mender_log_error("Unable to allocate memory");
        goto FAIL;
    }
    request.tmp_buf_len = MENDER_WEBSOCKET_RECV_BUF_LENGTH;

    /* Add headers */
    if (MENDER_FAIL == header_add(header_fields, header_fields_size, MENDER_HEADER_WEBSOCKET_USER_AGENT)) {
        mender_log_error("Unable to add 'User-Agent' header");
        goto FAIL;
    }

    if (NULL != jwt) {
        auth_header = header_alloc_and_add(header_fields, header_fields_size, "Authorization: Bearer %s\r\n", jwt);
        if (NULL == auth_header) {
            mender_log_error("Unable to add 'Authorization' header");
            goto FAIL;
        }
    }
    request.optional_headers = header_fields;

    /* Connect to the server */
    ((mender_websocket_handle_t *)*handle)->sock = mender_net_connect(host, port);
    if (((mender_websocket_handle_t *)*handle)->sock < 0) {
        mender_log_error("Unable to open HTTP client connection");
        goto FAIL;
    }

    /* Upgrade to WebSocket connection */
    if ((((mender_websocket_handle_t *)*handle)->client
         = websocket_connect(((mender_websocket_handle_t *)*handle)->sock, &request, CONFIG_MENDER_WEBSOCKET_CONNECT_TIMEOUT, NULL))
        < 0) {
        mender_log_error("Unable to upgrade to websocket connection");
        goto FAIL;
    }
    if (MENDER_OK != (ret = callback(MENDER_WEBSOCKET_EVENT_CONNECTED, NULL, 0, params))) {
        mender_log_error("An error occurred during the websocket connected callback");
        goto FAIL;
    }

    /* Create and start websocket thread */
    k_thread_create(&((mender_websocket_handle_t *)*handle)->thread_handle,
                    mender_websocket_thread_stack,
                    CONFIG_MENDER_WEBSOCKET_THREAD_STACK_SIZE * 1024,
                    mender_websocket_thread,
                    *handle,
                    NULL,
                    NULL,
                    CONFIG_MENDER_WEBSOCKET_THREAD_PRIORITY,
                    0,
                    K_NO_WAIT);
    k_thread_name_set(&((mender_websocket_handle_t *)*handle)->thread_handle, "mender_websocket");

    ret = MENDER_OK;
    goto END;

FAIL:

    /* Close connection and release memory */
    if (NULL != *handle) {
        if (((mender_websocket_handle_t *)*handle)->sock > 0) {
            mender_net_disconnect(((mender_websocket_handle_t *)*handle)->sock);
        }
        free(*handle);
        *handle = NULL;
    }

END:

    /* Release memory */
    free(host);
    free(port);
    free(url);

    free(auth_header);
    free(request.tmp_buf);

    return ret;
}

mender_err_t
mender_websocket_send(void *handle, void *payload, size_t length) {

    assert(NULL != handle);
    assert(NULL != payload);
    int sent;

    /* Send binary payload */
    if (length
        != (sent = websocket_send_msg(((mender_websocket_handle_t *)handle)->client,
                                      payload,
                                      length,
                                      WEBSOCKET_OPCODE_DATA_BINARY,
                                      true,
                                      true,
                                      CONFIG_MENDER_WEBSOCKET_REQUEST_TIMEOUT))) {
        mender_log_error("Unable to send data over websocket connection: %d", sent);
        return MENDER_FAIL;
    }

    return MENDER_OK;
}

mender_err_t
mender_websocket_disconnect(void *handle) {

    assert(NULL != handle);

    /* Close websocket connection */
    ((mender_websocket_handle_t *)handle)->abort = true;
    websocket_disconnect(((mender_websocket_handle_t *)handle)->client);

    /* Wait end of execution of the websocket thread */
    k_thread_join(&((mender_websocket_handle_t *)handle)->thread_handle, K_FOREVER);

    /* Release memory */
    free(handle);

    return MENDER_OK;
}

mender_err_t
mender_websocket_exit(void) {

    /* Nothing to do */
    return MENDER_OK;
}

static void
mender_websocket_thread(void *p1, void *p2, void *p3) {

    assert(NULL != p1);
    mender_websocket_handle_t *handle = (mender_websocket_handle_t *)p1;
    (void)p2;
    (void)p3;
    uint8_t *payload;
    int      received;
    uint32_t message_type = 0;
    uint64_t remaining    = 0;

    /* Allocate payload */
    if (NULL == (payload = (uint8_t *)malloc(MENDER_WEBSOCKET_RECV_BUF_LENGTH))) {
        mender_log_error("Unable to allocate memory");
        goto END;
    }

    /* Perform reception of data from the websocket connection */
    while (false == handle->abort) {

        received = websocket_recv_msg(handle->client, payload, MENDER_WEBSOCKET_RECV_BUF_LENGTH, &message_type, &remaining, SYS_FOREVER_MS);
        if (received < 0) {
            if (-ENOTCONN == received) {
                mender_log_error("Connection has been closed");
                goto END;
            }
            mender_log_error("Unable to receive websocket message: errno=%d", errno);

        } else if (received > 0) {

            /* Perform treatment depending of the opcode */
            if (WEBSOCKET_FLAG_PING == (message_type & WEBSOCKET_FLAG_PING)) {

                /* Send pong message with the same payload */
                websocket_send_msg(handle->client, payload, received, WEBSOCKET_OPCODE_PONG, true, true, CONFIG_MENDER_WEBSOCKET_REQUEST_TIMEOUT);

            } else if (WEBSOCKET_FLAG_BINARY == (message_type & WEBSOCKET_FLAG_BINARY)) {

                /* Invoke callback */
                if (MENDER_OK != handle->callback(MENDER_WEBSOCKET_EVENT_DATA_RECEIVED, payload, received, handle->params)) {
                    mender_log_error("An error occurred");
                }
            }
        }
    }

END:

    /* Invoke disconnected callback */
    handle->callback(MENDER_WEBSOCKET_EVENT_DISCONNECTED, NULL, 0, handle->params);

    /* Release memory */
    free(payload);
}
