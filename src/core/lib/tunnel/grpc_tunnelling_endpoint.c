/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "grpc_tunnelling_endpoint.h"

#include <stdio.h>
#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/slice.h>
#include <grpc/support/string_util.h>

/* TODO(gnirodi): DO NOT SUBMIT */
char * kPeer = "peer";

typedef struct {
  /* Wraps the tunnel's vtable to the endpoint */
  grpc_endpoint base;
  /* The call supplied during creation used for tunneling */
  grpc_call *call;

  /* status of this tunnel */
  enum grpc_tunnel_status {
    TUNNEL_NEW = 1,
    TUNNEL_CONNECT_IN_PROGRESS = 2,
    TUNNEL_ESTABLISHED = 3,
    TUNNEL_TTL_2_LAMEDUCK_NOTIFIED = 4,
    TUNNEL_IN_LAMEDUCK = 5,
    TUNNEL_CLOSED = 6
  } tunnel_status;

  /* ref counts instrumental for destruction */
  gpr_refcount refcount;

  /* tunnel connection related members */
  grpc_op connect_ops[1];
  // The receipt of the initial metadata, notifies caller of tunnel_create()
  grpc_closure *connect_cb;
  grpc_closure on_connect;

  /* set to true once initial_metadata_to_recieve has been populated */
	bool is_initial_metadata_recived;
	/* initial meta data received by this endpoint */
	grpc_metadata_array *initial_metadata_to_receive;

  /* tunnel disconnection related members */
  grpc_op disconnect_ops[2];
  grpc_closure on_disconnect;

  /* initial meta data sent by this endpoint, owned by the tunnel */
	grpc_metadata_array* initial_metadata_to_send;

  /* trailing meta data sent if this call pertains to a server or received
   * if this call pertains to a client
   */
  grpc_metadata_array* trailing_metadata;

  grpc_status_code status_code;
  char* status_details;
  size_t status_details_capacity;
  int was_cancelled;

  /* tunnel_read related members */
  int read_ops_tag;
  grpc_op read_ops[6];
  grpc_closure *read_cb;
  grpc_closure on_read;
  gpr_slice_buffer *incoming_buffer;
  // When a read occurs, notifies caller of tunnel_read()

  /* tunnel_write related members */
  int write_ops_tag;
  grpc_op write_ops[6];
  grpc_closure *write_cb;
  grpc_closure on_write;
  gpr_slice_buffer *outgoing_buffer;
  // When all writes are done, notifies caller of tunnel_write()

  /* TODO(gnirodi): Not sure yet what this has */
  char *peer_string;
} grpc_tunnel;

static const int kClientWriteOpsTagStart = 1;
static const int kClientReadOpsTagStart = 2;
static const int kServerWriteOpsTagStart = 3;
static const int kServerReadOpsTagStart = 4;
static const int kOpsTagIncrement = 4;

/* Forward declaration */
static const grpc_endpoint_vtable vtable;

static void tunnel_free(grpc_tunnel *tunnel) {
  gpr_free(tunnel);
}

static void tunnel_unref(grpc_tunnel *tunnel) {
  if (gpr_unref(&tunnel->refcount)) {
	  tunnel_free(tunnel);
  }
}

static char *tunnel_get_peer(grpc_endpoint *ep) {
  GPR_ASSERT(ep->vtable == &vtable);
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  return gpr_strdup(tunnel->peer_string);
}

static void tunnel_read(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
						gpr_slice_buffer *incoming_buffer, grpc_closure *cb) {
  GPR_ASSERT(ep->vtable == &vtable);
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  GPR_ASSERT(tunnel->read_cb == NULL);
  tunnel->read_cb = cb;
  tunnel->incoming_buffer = incoming_buffer;
  /* TODO(gnirodi): do read using the call */
}

static void tunnel_write(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
						 gpr_slice_buffer *buf, grpc_closure *cb) {
  GPR_ASSERT(ep->vtable == &vtable);
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  GPR_ASSERT(tunnel->write_cb == NULL);
  if (buf->length == 0) {
    grpc_exec_ctx_enqueue(exec_ctx, cb, true, NULL);
    return;
  }
  tunnel->outgoing_buffer = buf;
  /* TODO(gnirodi): do write using the call */
}

static void tunnel_add_to_pollset(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                  grpc_pollset *pollset) {
  GPR_ASSERT(ep->vtable == &vtable);
  // Do nothing. This endpoint does not interact with file descriptors
}

static void tunnel_add_to_pollset_set(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                   grpc_pollset_set *pollset_set) {
  GPR_ASSERT(ep->vtable == &vtable);
  // Do nothing. This endpoint does not interact with file descriptors
}

static void tunnel_shutdown(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep) {
  GPR_ASSERT(ep->vtable == &vtable);
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  /* TODO(gnirodi): possibly use call to shutdown peer */
  /* TODO(gnirodi): DO NOT SUBMIT */
  GPR_ASSERT(tunnel != NULL);
}

static void tunnel_destroy(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep) {
  GPR_ASSERT(ep->vtable == &vtable);
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  tunnel_unref(tunnel);
}

static const grpc_endpoint_vtable vtable = {
  tunnel_read, tunnel_write, tunnel_add_to_pollset,
  tunnel_add_to_pollset_set, tunnel_shutdown, tunnel_destroy,
  tunnel_get_peer
};

static void on_read(grpc_exec_ctx *exec_ctx, void *tunnelp, bool success) {
  grpc_tunnel *tunnel = (grpc_tunnel *)tunnelp;
  GPR_ASSERT(tunnel->base.vtable == &vtable);
  // TODO(gnirodi)
}

static void on_write(grpc_exec_ctx *exec_ctx, void *tunnelp, bool success) {
  grpc_tunnel *tunnel = (grpc_tunnel *)tunnelp;
  GPR_ASSERT(tunnel->base.vtable == &vtable);
  // TODO(gnirodi)
}

static void on_connect(
		grpc_exec_ctx *exec_ctx, void *tunnelp, bool success) {
  grpc_tunnel *tunnel = (grpc_tunnel *)tunnelp;
  GPR_ASSERT(tunnel->base.vtable == &vtable);
  if (!success) {
  	tunnel->tunnel_status = TUNNEL_CLOSED;
  	return;
  }
	tunnel->tunnel_status = TUNNEL_ESTABLISHED;

}

static void on_disconnect(
		grpc_exec_ctx *exec_ctx, void *tunnelp, bool success) {
  grpc_tunnel *tunnel = (grpc_tunnel *)tunnelp;
  GPR_ASSERT(tunnel->base.vtable == &vtable);
  tunnel->tunnel_status = TUNNEL_CLOSED;
  if (!success) {
  	return;
  }
}

grpc_endpoint *grpc_tunnelling_endpoint_create(
		grpc_exec_ctx *exec_ctx,
		grpc_call *call,
		grpc_metadata_array *initial_metadata_to_send,
		grpc_metadata_array *initial_metadata_to_receive,
		grpc_metadata_array *trailing_metadata,
		grpc_closure *notify_on_connect_cb) {
  grpc_tunnel *tunnel = (grpc_tunnel *)gpr_malloc(sizeof(grpc_tunnel));
  memset(tunnel, 0, sizeof(*tunnel));
  tunnel->base.vtable = &vtable;
  tunnel->call = call;
  tunnel->peer_string = kPeer;
  tunnel->initial_metadata_to_send = initial_metadata_to_send;
  tunnel->initial_metadata_to_receive = initial_metadata_to_receive;
  tunnel->trailing_metadata = trailing_metadata;
  tunnel->connect_cb = notify_on_connect_cb;
  grpc_closure_init(&tunnel->on_connect, on_connect, tunnel);
  grpc_closure_init(&tunnel->on_disconnect, on_disconnect, tunnel);
  grpc_closure_init(&tunnel->on_read, on_read, tunnel);
  grpc_closure_init(&tunnel->on_write, on_write, tunnel);
  tunnel->tunnel_status = TUNNEL_NEW;

	uint8_t is_client_grpc_call = grpc_call_is_client(call);
  if (is_client_grpc_call) {
		tunnel->write_ops_tag = kClientWriteOpsTagStart;
		tunnel->read_ops_tag = kClientReadOpsTagStart;
  } else {
		tunnel->write_ops_tag = kServerWriteOpsTagStart;
		tunnel->read_ops_tag = kServerReadOpsTagStart;
  }
  tunnel->write_ops_tag += kOpsTagIncrement;

  /* paired with unref in grpc_tcp_destroy */
  gpr_ref_init(&tunnel->refcount, 1);


  // call start batch for disconnectivity
  grpc_op* connect_op = tunnel->disconnect_ops;
  connect_op->op = GRPC_OP_SEND_INITIAL_METADATA;
  connect_op->data.send_initial_metadata.count =
  		initial_metadata_to_send->count;
  connect_op->data.send_initial_metadata.metadata =
  		initial_metadata_to_send->metadata;
  connect_op->flags = 0;
  connect_op->reserved = NULL;
  connect_op++;
  tunnel->status_code = GRPC_STATUS__DO_NOT_USE;
  tunnel->status_details = NULL;
  tunnel->status_details_capacity = 0;
  tunnel->was_cancelled = 2;
  connect_op++;
	if (is_client_grpc_call) {
		connect_op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
    connect_op->data.recv_status_on_client.trailing_metadata =
    		tunnel->trailing_metadata;
    connect_op->data.recv_status_on_client.status = &tunnel->status_code;
    connect_op->data.recv_status_on_client.status_details =
    		&tunnel->status_details;
    connect_op->data.recv_status_on_client.status_details_capacity =
    		&tunnel->status_details_capacity;
  } else {
    connect_op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
    connect_op->data.recv_close_on_server.cancelled = &tunnel->was_cancelled;
  }
  connect_op->flags = 0;
  connect_op->reserved = NULL;
  connect_op++;
  grpc_call_start_batch_and_execute(
  		exec_ctx, call, tunnel->disconnect_ops,
  		(size_t)(connect_op - &tunnel->disconnect_ops[0]),
			&tunnel->on_disconnect);

  // call start batch for connectivity
  connect_op = tunnel->connect_ops;
  connect_op->op = GRPC_OP_RECV_INITIAL_METADATA;
  connect_op->data.recv_initial_metadata = tunnel->initial_metadata_to_receive;
  connect_op->flags = 0;
  connect_op->reserved = NULL;
  connect_op++;
  grpc_call_start_batch_and_execute(
  		exec_ctx, call, tunnel->connect_ops,
  		(size_t)(connect_op - &tunnel->connect_ops[0]),
			&tunnel->on_connect);

  tunnel->tunnel_status = TUNNEL_CONNECT_IN_PROGRESS;
  return &tunnel->base;
}
