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

#include "src/core/lib/tunnel/grpc_tunnel.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/slice.h>
#include <grpc/support/string_util.h>
#include "src/core/lib/channel/channel_args.h"

/* TODO(gnirodi): DO NOT SUBMIT */
char * kPeer = "peer";


enum grpc_tunnel_status {
  TUNNEL_NEW = 1,
  TUNNEL_CONNECT_IN_PROGRESS = 2,
  TUNNEL_ESTABLISHED = 3,
  TUNNEL_TTL_2_LAMEDUCK_NOTIFIED = 4,
  TUNNEL_IN_LAMEDUCK = 5,
  TUNNEL_CLOSED = 6
};


typedef struct {
  /* Wraps the tunnel's vtable to the endpoint */
  grpc_endpoint base;
  /* The call supplied during creation used for tunneling */
  grpc_call *call;

  /* initial meta data sent by this endpoint, owned by the tunnel */
	grpc_metadata_array initial_metadata_to_send;
	/* initial meta data received by this endpoint */
	grpc_metadata_array *initial_metadata_to_receive;
	/* set to true once initial_metadata_to_recieve has been populated */
	bool is_initial_metadata_recived;
	/* trailing meta data sent if this call pertains to a server or received
   * if this call pertains to a client
   */
  grpc_metadata_array* trailing_metadata;

  /* ref counts instrumental for destruction */
  gpr_refcount refcount;

  /* tunnel connection related members */
  grpc_op connect_ops_send[2];
  grpc_op connect_ops_recv;
  grpc_status_code status_code;
  char* status_details;
  size_t status_details_capacity;
  int was_cancelled;
  grpc_channel_args* tunnel_args;
  int millis_to_load_shed;
  int millis_to_lame_duck;
  int millis_to_shut_down;
  grpc_closure on_connect;

  /* tunnel_read related members */
  int read_ops_tag;
  grpc_op read_ops[6];
  grpc_closure *read_cb;
  gpr_slice_buffer *incoming_buffer;
  grpc_closure on_read;

  /* tunnel_write related members */
  int write_ops_tag;
  grpc_op write_ops[6];
  grpc_closure *write_cb;
  gpr_slice_buffer *outgoing_buffer;
  grpc_closure on_write;

  /* TODO(gnirodi): Not sure yet what this has */
  char *peer_string;
} grpc_tunnel;

/* GRPC tunneling headers */

/* Sent by the tunneling server call to notify of the number of milliseconds
 * from the time the client received this header, after which the tunnel must
 * establish a new channel for the purpose of load shedding.
 */
#define GRPC_ARG_TUNNEL_MILLIS_TO_LOAD_SHED \
  "grpc.tunnel.millis_to_loadshed"

/* Sent by the tunneling server call to notify of the number of milliseconds
 * from the time the client received this header, after which the tunneling
 * server call will stop sending messages across the tunnel.
 */
#define GRPC_ARG_TUNNEL_MILLIS_TO_LAME_DUCK \
  "grpc.tunnel.millis_to_lameduck"

/* Sent by the tunneling server call to notify of the number of milliseconds
 * from the time the client received this header, after which the tunneling
 * server call will shutdown the call.
 */
#define GRPC_ARG_TUNNEL_MILLIS_TO_SHUT_DOWN \
  "grpc.tunnel.millis_to_shutdown"

static const int kMillisToLoadshed = 240000;
static const int kMillisToLameduck = 260000;
static const int kMillisToShutdown = 300000;

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

static void* tag(intptr_t t) { return (void*)t; }

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
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  /* TODO(gnirodi): possibly tie the call to the pollset */
  /* TODO(gnirodi): DO NOT SUBMIT */
  GPR_ASSERT(tunnel != NULL);
}

static void tunnel_add_to_pollset_set(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                   grpc_pollset_set *pollset_set) {
  GPR_ASSERT(ep->vtable == &vtable);
  grpc_tunnel *tunnel = (grpc_tunnel *)ep;
  /* TODO(gnirodi): possibly tie the call to the pollset set */
  /* TODO(gnirodi): DO NOT SUBMIT */
  GPR_ASSERT(tunnel != NULL);
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

}

static void on_write(grpc_exec_ctx *exec_ctx, void *tunnelp, bool success) {

}

static void on_connect(grpc_exec_ctx *exec_ctx, void *tunnelp, bool success) {
  if (!success) {
  	return;
  }
	grpc_tunnel *tunnel = (grpc_tunnel *) tunnelp;
	size_t count_md = tunnel->initial_metadata_to_receive->count;
	for (int md_idx = 0; md_idx < count_md; md_idx++) {
		grpc_metadata* md = tunnel->initial_metadata_to_receive->metadata[md_idx];
		if (strncmp(GRPC_ARG_TUNNEL_MILLIS_TO_LOAD_SHED, md->key,
								strlen(GRPC_ARG_TUNNEL_MILLIS_TO_LOAD_SHED)) == 0) {
			if ()
		}
	}

}

static void build_tunnel_arg(grpc_tunnel* tunnel,
		const char* using_key, grpc_arg *argv) {
	grpc_arg load_shed_str_arg;
	load_shed_str_arg.key = using_key;
	load_shed_str_arg.type = GRPC_ARG_STRING;
	int val_len = asprintf(&load_shed_str_arg.value.string, "%d",
			argv->value.integer);
	grpc_channel_args* copied_args = grpc_channel_args_copy_and_add(
			tunnel->tunnel_args, &load_shed_str_arg, 1);
	grpc_channel_args_destroy(tunnel->tunnel_args);
	tunnel->tunnel_args = copied_args;
}

static void init_tunnel_args(grpc_tunnel* tunnel,
		grpc_channel_args *channel_args) {
	tunnel->tunnel_args = grpc_channel_args_copy_and_add(NULL, NULL, 0);
	int argc = channel_args->num_args;
	grpc_arg *argv = channel_args->args;
	for (int arg_idx = 0; arg_idx < argc; arg_idx++) {
		if (argv->type == GRPC_ARG_INTEGER) {
			if ((strncmp(argv->key, GRPC_ARG_TUNNEL_MILLIS_TO_LOAD_SHED,
					 strlen(GRPC_ARG_TUNNEL_MILLIS_TO_LOAD_SHED)) == 0)
					|| (strncmp(argv->key, GRPC_ARG_TUNNEL_MILLIS_TO_LAME_DUCK,
							 strlen(GRPC_ARG_TUNNEL_MILLIS_TO_LAME_DUCK)) == 0)
				  || (strncmp(argv->key, GRPC_ARG_TUNNEL_MILLIS_TO_SHUT_DOWN,
							 strlen(GRPC_ARG_TUNNEL_MILLIS_TO_SHUT_DOWN)) == 0)) {
				build_tunnel_arg(tunnel, argv->key, argv);
			}
		}
	}
}

grpc_endpoint *grpc_tunnel_create(
		grpc_call *call,
		grpc_metadata_array *initial_metadata_to_send,
		grpc_metadata_array *initial_metadata_to_receive,
		grpc_metadata_array *trailing_metadata,
		grpc_channel_args *channel_args) {
	uint8_t is_client_grpc_call = grpc_call_is_client(call);
  grpc_tunnel *tunnel = (grpc_tunnel *)gpr_malloc(sizeof(grpc_tunnel));
  tunnel->millis_to_load_shed = kMillisToLoadshed;
  tunnel->millis_to_lame_duck = kMillisToLameduck;
  tunnel->millis_to_shut_down = kMillisToShutdown;
  tunnel->base.vtable = &vtable;
  tunnel->call = call;
  tunnel->peer_string = kPeer;
  tunnel->initial_metadata_to_send = initial_metadata_to_send;
  tunnel->initial_metadata_to_receive = initial_metadata_to_receive;
  tunnel->trailing_metadata = trailing_metadata;
  grpc_closure_init(&tunnel->on_connect, on_connect, tunnel);
  grpc_closure_init(&tunnel->on_read, on_read, tunnel);
  grpc_closure_init(&tunnel->on_write, on_write, tunnel);

  /* paired with unref in grpc_tcp_destroy */
  gpr_ref_init(&tunnel->refcount, 1);
  grpc_op* connect_op = tunnel->connect_ops_send;
  grpc_metadata_array_init(&tunnel->initial_metadata_to_send);
  size_t count_md_to_alloc = initial_metadata_to_send->count;
  connect_op->op = GRPC_OP_SEND_INITIAL_METADATA;
  tunnel->initial_metadata_to_send.count =  initial_metadata_to_send->count;
  if (is_client_grpc_call) {
  	tunnel->initial_metadata_to_send.count += 3;
  }
  tunnel->initial_metadata_to_send.metadata = (grpc_metadata *)gpr_malloc(
  		sizeof(grpc_metadata) * tunnel->initial_metadata_to_send.count);
  tunnel->initial_metadata_to_send.capacity =
  		tunnel->initial_metadata_to_send.count;
  if (initial_metadata_to_send->count > 0) {
		memcpy(tunnel->initial_metadata_to_send.metadata,
				initial_metadata_to_send->metadata,
				sizeof(grpc_metadata) * initial_metadata_to_send->count);
  }
  grpc_metadata* init_md = &(tunnel
  		->initial_metadata_to_send.metadata[initial_metadata_to_send->count]);
  init_md->key = GRPC_ARG_TUNNEL_MILLIS_TO_LOAD_SHED;
  init_md->value = &tunnel->millis_to_load_shed;
  init_md->value_length = sizeof(int);
  init_md++;
  init_md->key = GRPC_ARG_TUNNEL_MILLIS_TO_LAME_DUCK;
  init_md->value = &tunnel->millis_to_lame_duck;
  init_md->value_length = sizeof(int);
  init_md++;
  init_md->key = GRPC_ARG_TUNNEL_MILLIS_TO_SHUT_DOWN;
  init_md->value = &tunnel->millis_to_shut_down;
  init_md->value_length = sizeof(int);

  connect_op->data.send_initial_metadata.count =
  		tunnel->initial_metadata_to_send.count;
  connect_op->data.send_initial_metadata.metadata =
  		initial_metadata_to_send->metadata;
  connect_op->flags = 0;
  connect_op->reserved = NULL;
  connect_op++;
  connect_op->op = GRPC_OP_RECV_INITIAL_METADATA;
  connect_op->data.recv_initial_metadata = tunnel->initial_metadata_to_receive;
  connect_op->flags = 0;
  connect_op->reserved = NULL;
  connect_op++;
  if (is_client_grpc_call) {
		tunnel->write_ops_tag = kClientWriteOpsTagStart;
		tunnel->read_ops_tag = kClientReadOpsTagStart;
  } else {
		tunnel->write_ops_tag = kServerWriteOpsTagStart;
		tunnel->read_ops_tag = kServerReadOpsTagStart;
  }
  grpc_call_start_batch(call, tunnel->connect_ops_send,
  		(size_t)(connect_op - tunnel->connect_ops_send),
			tag(tunnel->write_ops_tag), NULL);
  tunnel->write_ops_tag += kOpsTagIncrement;
  tunnel->status_code = GRPC_STATUS__DO_NOT_USE;
  tunnel->status_details = NULL;
  tunnel->status_details_capacity = 0;
  tunnel->was_cancelled = 2;
  connect_op = &tunnel->connect_ops_recv;
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
  grpc_call_start_batch_and_execute(call, &tunnel->connect_ops_recv,
  		(size_t)(connect_op - &tunnel->connect_ops_recv),
			tag(tunnel->read_ops_tag), &tunnel->on_connect);
  tunnel->read_ops_tag += kOpsTagIncrement;
  return &tunnel->base;
}
