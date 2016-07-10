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

#include <grpc/grpc.h>

#include <stdlib.h>
#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/slice.h>
#include <grpc/support/slice_buffer.h>

#include "grpc/impl/codegen/grpc_types.h"
#include "src/core/ext/client_config/client_channel.h"
#include "src/core/ext/client_config/resolver_registry.h"
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/compress_filter.h"
#include "src/core/lib/channel/http_client_filter.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/security/credentials/credentials.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/tunnel/grpc_tunnelling_endpoint.h"

static void *tag(intptr_t t) { return (void *)t; }

typedef struct {
  grpc_connector base;
  gpr_refcount refs;

  grpc_closure *notify;
  grpc_connect_in_args args;
  grpc_connect_out_args *result;
  grpc_closure initial_string_sent;
  gpr_slice_buffer initial_string_buffer;

	// binding supplied to this channel via grpc_channel_add_tunnel_binding()
	// The binding is not owned by the tunnel_connector
	grpc_tunnel_channel_binding *tunnel_channel_binding;

	// The binding call used as the transport for this server, akin to a
	// TCP socket.
	grpc_call *binding_call;

	grpc_call_details call_details;
	grpc_metadata_array initial_metadata_to_send;
	grpc_metadata_array initial_metadata_to_receive;
	grpc_metadata_array trailing_metadata;

	grpc_endpoint *tunneling_endpoint;

	grpc_closure on_endpoint_available;
} tunnel_connector;

static gpr_timespec get_timeout_to_millis(int x) {
	return gpr_time_add(
			gpr_now(GPR_CLOCK_MONOTONIC),
			gpr_time_from_micros((int64_t)(1e3 * (x)),
													 GPR_TIMESPAN));
}

static void tunnel_connector_ref(grpc_connector *con) {
  tunnel_connector *c = (tunnel_connector *)con;
  gpr_ref(&c->refs);
}

static void tunnel_connector_unref(
		grpc_exec_ctx *exec_ctx, grpc_connector *con) {
  tunnel_connector *c = (tunnel_connector *)con;
  if (gpr_unref(&c->refs)) {
    /* c->initial_string_buffer does not need to be destroyed */
    gpr_free(c);
  }
}

static void on_initial_connect_string_sent(grpc_exec_ctx *exec_ctx, void *arg,
                                           bool success) {
  tunnel_connector_unref(exec_ctx, arg);
}

static void on_endpoint_available(
		grpc_exec_ctx *exec_ctx, void *arg, bool success) {
  tunnel_connector *c = arg;
  grpc_closure *notify;
  grpc_endpoint *tunnel_bound_endpoint = c->tunneling_endpoint;
  if (tunnel_bound_endpoint != NULL) {
    if (!GPR_SLICE_IS_EMPTY(c->args.initial_connect_string)) {
      grpc_closure_init(&c->initial_string_sent, on_initial_connect_string_sent,
                        c);
      gpr_slice_buffer_init(&c->initial_string_buffer);
      gpr_slice_buffer_add(&c->initial_string_buffer,
                           c->args.initial_connect_string);
      tunnel_connector_ref(arg);
      grpc_endpoint_write(
      		exec_ctx,
					tunnel_bound_endpoint,
      		&c->initial_string_buffer,
          &c->initial_string_sent);
    }
    c->result->transport = grpc_create_chttp2_transport(
    		exec_ctx, c->args.channel_args, tunnel_bound_endpoint, 1);
    grpc_chttp2_transport_start_reading(
    		exec_ctx, c->result->transport, NULL, 0);
    GPR_ASSERT(c->result->transport);
    c->result->channel_args = c->args.channel_args;
  } else {
    memset(c->result, 0, sizeof(*c->result));
  }
  notify = c->notify;
  c->notify = NULL;
  notify->cb(exec_ctx, notify->cb_arg, 1);
}

static void tunnel_connector_shutdown(
		grpc_exec_ctx *exec_ctx, grpc_connector *con) {
	// TODO(gnirodi) investigate if the tunnel needs to cleanup
}

static void tunnel_connector_connect(
		grpc_exec_ctx *exec_ctx, grpc_connector *con,
		const grpc_connect_in_args *args, grpc_connect_out_args *result,
    grpc_closure *notify) {
  tunnel_connector *c = (tunnel_connector *)con;
  GPR_ASSERT(c->notify == NULL);
  GPR_ASSERT(notify->cb);
  c->notify = notify;
  c->args = *args;
  c->result = result;
  grpc_closure_init(&c->on_endpoint_available, on_endpoint_available, c);



  grpc_call_error error = 	grpc_server_request_call(
			c->tunnel_channel_binding->channel_bound_server,
			&c->binding_call, &c->call_details, &c->initial_metadata_to_send,
			c->tunnel_channel_binding->channel_binding_queue,
			c->tunnel_channel_binding->channel_binding_queue, tag(100));
  GPR_ASSERT(GRPC_CALL_OK == error);

  c->tunneling_endpoint = grpc_tunnelling_endpoint_create(
			exec_ctx, c->binding_call, &c->initial_metadata_to_send,
			&c->initial_metadata_to_receive, &c->trailing_metadata,
			&c->on_endpoint_available);
}

static const grpc_connector_vtable tunnel_connector_vtable = {
    tunnel_connector_ref, tunnel_connector_unref, tunnel_connector_shutdown,
		tunnel_connector_connect
};

typedef struct {
  grpc_client_channel_factory base;
  gpr_refcount refs;
  grpc_channel_args *merge_args;

  // The tunnel_bound_channel_factory does not own the binding
  grpc_tunnel_channel_binding* tunnel_channel_binding;
  grpc_channel *master;
} tunnel_bound_channel_factory;

static void tunnel_bound_channel_factory_ref(
    grpc_client_channel_factory *cc_factory) {
  tunnel_bound_channel_factory *f = (tunnel_bound_channel_factory *)cc_factory;
  gpr_ref(&f->refs);
}

static void tunnel_bound_channel_factory_unref(
    grpc_exec_ctx *exec_ctx, grpc_client_channel_factory *cc_factory) {
  tunnel_bound_channel_factory *f = (tunnel_bound_channel_factory *)cc_factory;
  if (gpr_unref(&f->refs)) {
    if (f->master != NULL) {
      GRPC_CHANNEL_INTERNAL_UNREF(exec_ctx, f->master,
                                  "tunnel_bound_channel_factory");
    }
    grpc_channel_args_destroy(f->merge_args);
    gpr_free(f);
  }
}

static grpc_subchannel *tunnel_bound_channel_factory_create_subchannel(
    grpc_exec_ctx *exec_ctx, grpc_client_channel_factory *cc_factory,
    grpc_subchannel_args *args) {
  tunnel_bound_channel_factory *f = (tunnel_bound_channel_factory *)cc_factory;
  tunnel_connector *c = gpr_malloc(sizeof(*c));
  grpc_channel_args *final_args =
      grpc_channel_args_merge(args->args, f->merge_args);
  grpc_subchannel *s;
  memset(c, 0, sizeof(*c));
  c->base.vtable = &tunnel_connector_vtable;
  c->tunnel_channel_binding = f->tunnel_channel_binding;
  gpr_ref_init(&c->refs, 1);
  args->args = final_args;
  s = grpc_subchannel_create(exec_ctx, &c->base, args);
  tunnel_connector_unref(exec_ctx, &c->base);
  grpc_channel_args_destroy(final_args);
  return s;
}

static grpc_channel *tunnel_bound_channel_factory_create_channel(
    grpc_exec_ctx *exec_ctx, grpc_client_channel_factory *cc_factory,
    const char *target, grpc_client_channel_type type,
    grpc_channel_args *args) {
  tunnel_bound_channel_factory *f = (tunnel_bound_channel_factory *)cc_factory;
  grpc_channel_args *final_args = grpc_channel_args_merge(args, f->merge_args);
  grpc_channel *channel = grpc_channel_create(exec_ctx, target, final_args,
                                              GRPC_CLIENT_CHANNEL, NULL);
  grpc_channel_args_destroy(final_args);
  grpc_resolver *resolver = grpc_resolver_create(target, &f->base);
  if (!resolver) {
    GRPC_CHANNEL_INTERNAL_UNREF(exec_ctx, channel,
                                "tunnel_bound_channel_factory_create_channel");
    return NULL;
  }

  grpc_client_channel_set_resolver(
      exec_ctx, grpc_channel_get_channel_stack(channel), resolver);
  GRPC_RESOLVER_UNREF(exec_ctx, resolver, "create_channel");

  return channel;
}

static const grpc_client_channel_factory_vtable
		tunnel_bound_channel_factory_vtable = {
				tunnel_bound_channel_factory_ref,
				tunnel_bound_channel_factory_unref,
				tunnel_bound_channel_factory_create_subchannel,
				tunnel_bound_channel_factory_create_channel
};

/* Create a tunnel bound channel:
   Asynchronously: - resolve target
                   - connect to it (trying alternatives as presented)
                   - perform handshakes
   TODO(gnirodi) This ought to be secure!! */
grpc_channel *grpc_tunnel_channel_create( grpc_tunnel_channel_binding *binding,
																					const char *target,
                                          const grpc_channel_args *args,
                                          void *reserved) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  GRPC_API_TRACE(
      "grpc_insecure_channel_create(target=%p, args=%p, reserved=%p)", 3,
      (target, args, reserved));
  GPR_ASSERT(!reserved);

  tunnel_bound_channel_factory *f = gpr_malloc(sizeof(*f));
  memset(f, 0, sizeof(*f));
  f->base.vtable = &tunnel_bound_channel_factory_vtable;
  f->tunnel_channel_binding = binding;
  gpr_ref_init(&f->refs, 1);
  f->merge_args = grpc_channel_args_copy(args);

  grpc_channel *channel = tunnel_bound_channel_factory_create_channel(
      &exec_ctx, &f->base, target, GRPC_CLIENT_CHANNEL_TYPE_REGULAR, NULL);
  if (channel != NULL) {
    f->master = channel;
    GRPC_CHANNEL_INTERNAL_REF(f->master, "grpc_tunnel_channel_create");
  }
  tunnel_bound_channel_factory_unref(&exec_ctx, &f->base);

  grpc_exec_ctx_finish(&exec_ctx);

  return channel != NULL ? channel : grpc_lame_client_channel_create(
                                         target, GRPC_STATUS_INTERNAL,
                                         "Failed to create tunnel channel");
}

// *****************  Tunnel Binding related *******************

grpc_tunnel_channel_binding *grpc_create_tunnel_channel_binding(
		grpc_channel_args *tunnel_binding_args, void *reserved) {
	grpc_tunnel_channel_binding *binding =
			gpr_malloc(sizeof(grpc_tunnel_channel_binding));
  memset(binding, 0, sizeof(*binding));
	binding->channel_bound_server =
			grpc_server_create(tunnel_binding_args, reserved);
	return binding;
}

void grpc_shutdown_tunnel_channel_binding(
		grpc_tunnel_channel_binding *tunnel_channel_binding,
		grpc_completion_queue* channel_binding_queue) {
  if (!tunnel_channel_binding->channel_bound_server) {
  	return;
  }
  grpc_server_shutdown_and_notify(tunnel_channel_binding->channel_bound_server,
  		channel_binding_queue, tag(1000));
  GPR_ASSERT(grpc_completion_queue_pluck( channel_binding_queue, tag(1000),
			get_timeout_to_millis(5000), NULL).type == GRPC_OP_COMPLETE);
  grpc_server_destroy(tunnel_channel_binding->channel_bound_server);
  tunnel_channel_binding->channel_bound_server = NULL;
}

void grpc_destroy_tunnel_channel_binding(
		grpc_tunnel_channel_binding *tunnel_channel_binding) {
	gpr_free(tunnel_channel_binding);
}

void grpc_tunnel_channel_binding_register_completion_queue(
		grpc_tunnel_channel_binding* tunnel_channel_binding,
		grpc_completion_queue* channel_binding_queue, void* reserved) {
	GPR_ASSERT(!tunnel_channel_binding->channel_binding_queue);
	tunnel_channel_binding->channel_binding_queue = channel_binding_queue;
	grpc_server_register_completion_queue(
			tunnel_channel_binding->channel_bound_server, channel_binding_queue,
			NULL);
}

int grpc_tunnel_channel_binding_add_insecure_http2_port(
		grpc_tunnel_channel_binding* tunnel_channel_binding,
		char* local_binding_address) {
	return grpc_server_add_insecure_http2_port(
			tunnel_channel_binding->channel_bound_server, local_binding_address);
}

int grpc_tunnel_channel_binding_add_secure_http2_port(
		grpc_tunnel_channel_binding* tunnel_channel_binding,
		char* local_binding_address, grpc_server_credentials* tunnel_creds) {
	return grpc_server_add_secure_http2_port(
			tunnel_channel_binding->channel_bound_server, local_binding_address,
			tunnel_creds);
}

void grpc_tunnel_channel_binding_start(
		grpc_tunnel_channel_binding* tunnel_channel_binding) {
  grpc_server_start(tunnel_channel_binding->channel_bound_server);
}
