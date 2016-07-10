
#include <string.h>
#include <grpc/grpc.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "grpc/impl/codegen/grpc_types.h"
#include "grpc/impl/codegen/log.h"
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/pollset.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/server.h"
#include "src/core/lib/tunnel/grpc_tunnelling_endpoint.h"

typedef struct {
	// The GRPC server this tunnel_bound_server serves.
	grpc_server *server;

	// binding supplied to this server via grpc_server_add_tunnel_binding()
	// The binding is not owned by the tunnel_bound_server
	grpc_tunnel_server_binding *binding;

	// The queue to use to obtain the binding call.
	grpc_completion_queue *binding_cq;

	// The binding call used as the transport for this server, akin to a
	// TCP socket.
	grpc_call *binding_call;

	grpc_metadata_array initial_metadata_to_send;
	grpc_metadata_array initial_metadata_to_receive;
	grpc_metadata_array trailing_metadata;

	grpc_endpoint *tunneling_endpoint;

	grpc_closure on_endpoint_available;

  /* all pollsets interested in new connections */
  grpc_pollset **pollsets;
  /* number of pollsets in the pollsets array */
  size_t pollset_count;

  /* next pollset to assign a channel to */
  size_t next_pollset_to_assign;

} tunnel_bound_server;

static gpr_timespec get_timeout(int timeout_millis) {
	return gpr_time_add(
			gpr_now(GPR_CLOCK_MONOTONIC),
			gpr_time_from_micros((int64_t)(1e3 * (timeout_millis)), GPR_TIMESPAN));
}

static void on_endpoint_available(
		grpc_exec_ctx *exec_ctx, void *bound_serverp, bool success) {
  tunnel_bound_server *tunneling_server = bound_serverp;

  if(!success) {
  	gpr_log(GPR_ERROR, "Tunneling server could not establish endpoint");
  	return;
  }
	gpr_log(GPR_ERROR, "Tunneling server established endpoint!");

  /*
   * Beware that the call to grpc_create_chttp2_transport() has to happen before
   * grpc_tcp_server_destroy(). This is fine here, but similar code
   * asynchronously doing a handshake instead of calling grpc_tcp_server_start()
   * (as in server_secure_chttp2.c) needs to add synchronization to avoid this
   * case.
   */
  grpc_transport *transport = grpc_create_chttp2_transport(
      exec_ctx,
			grpc_server_get_channel_args(tunneling_server->server),
			tunneling_server->tunneling_endpoint,
			0);
  grpc_pollset *accepting_pollset =
  		tunneling_server->pollsets[(tunneling_server->next_pollset_to_assign++) %
																 tunneling_server->pollset_count];
  grpc_server_setup_transport(
  		exec_ctx, tunneling_server->server,
			transport,
			accepting_pollset,
  		grpc_server_get_channel_args(tunneling_server->server));
  grpc_chttp2_transport_start_reading(exec_ctx, transport, NULL, 0);
}

/* Server callback: start listening on our tunnel */
static void start(grpc_exec_ctx *exec_ctx, grpc_server *server,
		void *listener_arg, grpc_pollset **pollsets, size_t pollset_count) {
  tunnel_bound_server *tunneling_server = listener_arg;
  tunneling_server->pollsets = pollsets;
  tunneling_server->pollset_count = pollset_count;
  tunneling_server->next_pollset_to_assign = 0;
  char *binding_target = grpc_channel_get_target(
  		tunneling_server->binding->server_bound_channel);
  tunneling_server->binding_call = grpc_channel_create_call(
  		tunneling_server->binding->server_bound_channel,
			NULL, GRPC_PROPAGATE_DEFAULTS, tunneling_server->binding_cq,
      "/tunnel", binding_target, get_timeout(5000), NULL);
  GPR_ASSERT(tunneling_server->binding_call);

  grpc_metadata_array_init(&tunneling_server->initial_metadata_to_send);
  grpc_metadata_array_init(&tunneling_server->initial_metadata_to_receive);
  grpc_metadata_array_init(&tunneling_server->trailing_metadata);

  grpc_closure_init(&tunneling_server->on_endpoint_available,
  		on_endpoint_available, tunneling_server);

  tunneling_server->tunneling_endpoint =
  		grpc_tunnelling_endpoint_create(exec_ctx, tunneling_server->binding_call,
  				&tunneling_server->initial_metadata_to_send,
					&tunneling_server->initial_metadata_to_receive,
					&tunneling_server->trailing_metadata,
					&tunneling_server->on_endpoint_available);
}

/* Server callback: destroy the tunnel listener (so we don't generate further
   callbacks) */
static void destroy(grpc_exec_ctx *exec_ctx, grpc_server *server,
		void *listener_arg, grpc_closure *destroy_done) {
  tunnel_bound_server *tunneling_server = listener_arg;
  GPR_ASSERT(tunneling_server);

  // TODO(gnirodi): Possibly destroy bound call.
  // grpc_tcp_server_unref(exec_ctx, tunneling_server);
  grpc_exec_ctx_enqueue(exec_ctx, destroy_done, true, NULL);
}

void grpc_server_add_tunnel_binding(
		grpc_server *server,
		grpc_tunnel_server_binding *binding,
		grpc_completion_queue *binding_cq) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  tunnel_bound_server *tunnel_server = (tunnel_bound_server *)
  		gpr_malloc(sizeof(tunnel_bound_server));
  memset(tunnel_server, 0, sizeof(*tunnel_server));
  tunnel_server->server = server;
  tunnel_server->binding = binding;
  tunnel_server->binding_cq = binding_cq;

  /* Register with the server only upon success */
  grpc_server_add_listener(&exec_ctx, server, tunnel_server, start, destroy);
	grpc_exec_ctx_finish(&exec_ctx);
}

grpc_tunnel_server_binding *grpc_create_tunnel_server_binding(
		const char *tunnel_target,
		const grpc_channel_args *tunnel_server_binding_args) {
	grpc_tunnel_server_binding *tunnel_server_binding =
  		gpr_malloc(sizeof(grpc_tunnel_server_binding));
  memset(tunnel_server_binding, 0, sizeof(*tunnel_server_binding));
  tunnel_server_binding->server_bound_channel = grpc_insecure_channel_create(
  		tunnel_target, tunnel_server_binding_args, NULL);
  GPR_ASSERT(tunnel_server_binding->server_bound_channel);
	return tunnel_server_binding;
}

void grpc_destroy_tunnel_server_binding(
		grpc_tunnel_server_binding *tunnel_server_binding) {
  grpc_channel_destroy(tunnel_server_binding->server_bound_channel);
  tunnel_server_binding->server_bound_channel = NULL;
	gpr_free(tunnel_server_binding);
}
