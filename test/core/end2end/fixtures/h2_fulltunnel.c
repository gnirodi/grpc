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

#include "test/core/end2end/end2end_tests.h"

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/thd.h>
#include <grpc/support/useful.h>
#include "grpc/impl/codegen/grpc_types.h"
#include "src/core/ext/client_config/client_channel.h"
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/channel/connected_channel.h"
#include "src/core/lib/channel/http_server_filter.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/server.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

typedef struct fulltunnelstack_fixture_data {
  char *tunnel_bind_addr;
  grpc_completion_queue *binding_cq;
  grpc_tunnel_channel_binding *tunnel_channel_binding;
  grpc_tunnel_server_binding *tunnel_server_binding;
} fulltunnelstack_fixture_data;

static grpc_end2end_test_fixture chttp2_create_fixture_fulltunnelstack(
    grpc_channel_args *client_args, grpc_channel_args *server_args) {
  grpc_end2end_test_fixture f;
  int bind_port = grpc_pick_unused_port_or_die();
  fulltunnelstack_fixture_data *ffd =
  		gpr_malloc(sizeof(fulltunnelstack_fixture_data));
  memset(&f, 0, sizeof(f));

  gpr_join_host_port(&ffd->tunnel_bind_addr, "localhost", bind_port);

  f.fixture_data = ffd;
  f.cq = grpc_completion_queue_create(NULL);

  // =========== Tunnel binding related initialization ===================
  // Create a tunnel channel binding and initialize
  ffd->tunnel_channel_binding = grpc_create_tunnel_channel_binding(
  		client_args, NULL);
  GPR_ASSERT(ffd->tunnel_channel_binding);
  ffd->binding_cq = grpc_completion_queue_create(NULL);
  grpc_tunnel_channel_binding_register_completion_queue(
  		ffd->tunnel_channel_binding, ffd->binding_cq, NULL);
  GPR_ASSERT(grpc_tunnel_channel_binding_add_insecure_http2_port(
  		ffd->tunnel_channel_binding, ffd->tunnel_bind_addr));
  grpc_tunnel_channel_binding_start(ffd->tunnel_channel_binding);

  // Create a tunnel server binding and initialize
  ffd->tunnel_server_binding = grpc_create_tunnel_server_binding(
  		ffd->tunnel_bind_addr, server_args);

  return f;
}

static gpr_timespec get_timeout_to_millis(int x) {
	return gpr_time_add(
			gpr_now(GPR_CLOCK_MONOTONIC),
			gpr_time_from_micros((int64_t)(1e3 * (x)),
													 GPR_TIMESPAN));
}

static void drain_cq(grpc_completion_queue *cq) {
  grpc_event ev;
  do {
    ev = grpc_completion_queue_next(cq, get_timeout_to_millis(5000), NULL);
  } while (ev.type != GRPC_QUEUE_SHUTDOWN);
}

void chttp2_init_client_fulltunnelstack(grpc_end2end_test_fixture *f,
                                  			grpc_channel_args *client_args) {
  fulltunnelstack_fixture_data *ffd = f->fixture_data;
  f->client = grpc_tunnel_channel_create(
  		ffd->tunnel_channel_binding, ffd->tunnel_bind_addr, client_args, NULL);
  GPR_ASSERT(f->client);
}

void chttp2_init_server_fulltunnelstack(grpc_end2end_test_fixture *f,
                                  			grpc_channel_args *server_args) {
  fulltunnelstack_fixture_data *ffd = f->fixture_data;
  if (f->server) {
    grpc_server_destroy(f->server);
  }
  f->server = grpc_server_create(server_args, NULL);
  GPR_ASSERT(f->server);
  grpc_server_register_completion_queue(f->server, f->cq, NULL);
  grpc_server_add_tunnel_binding(
  		f->server, ffd->tunnel_server_binding, ffd->binding_cq);
  grpc_server_start(f->server);
}

void chttp2_tear_down_fulltunnelstack(grpc_end2end_test_fixture *f) {
  fulltunnelstack_fixture_data *ffd = f->fixture_data;
  if (f->fixture_data) {
    // =========== Tunnel binding related tear down ===================
  	// Tear down tunnel server binding
  	grpc_destroy_tunnel_server_binding(ffd->tunnel_server_binding);

  	// Tear down tunnel channel binding
  	grpc_shutdown_tunnel_channel_binding(
  			ffd->tunnel_channel_binding, ffd->binding_cq);
		grpc_destroy_tunnel_channel_binding(ffd->tunnel_channel_binding);

		// Tear down any completion queue used for tunnel binding
    grpc_completion_queue_shutdown(ffd->binding_cq);
    drain_cq(ffd->binding_cq);
    grpc_completion_queue_destroy(ffd->binding_cq);

    gpr_free(ffd->tunnel_bind_addr);
    // =========== End Tunnel binding related tear down ===================

		gpr_free(ffd);
		f->fixture_data = NULL;
  }
}

/* All test configurations */
static grpc_end2end_test_config configs[] = {
    {"chttp2/fulltunnelstack", FEATURE_MASK_SUPPORTS_DELAYED_CONNECTION,
     chttp2_create_fixture_fulltunnelstack, chttp2_init_client_fulltunnelstack,
     chttp2_init_server_fulltunnelstack, chttp2_tear_down_fulltunnelstack},
};

int main(int argc, char **argv) {
  size_t i;

  grpc_test_init(argc, argv);
  grpc_end2end_tests_pre_init();
  grpc_init();

  for (i = 0; i < sizeof(configs) / sizeof(*configs); i++) {
    grpc_end2end_tests(argc, argv, configs[i]);
  }

  grpc_shutdown();

  return 0;
}


