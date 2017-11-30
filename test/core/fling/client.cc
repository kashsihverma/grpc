/*
 *
 * Copyright 2015 gRPC authors.
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
 *
 */

#include <grpc/grpc.h>

#include <stdio.h>
#include <string.h>

#include <gflags/gflags.h>

#include <grpc/support/histogram.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "src/core/lib/profiling/timers.h"
#include "test/core/util/grpc_profiler.h"
#include "test/core/util/test_config.h"

// In some distros, gflags is in the namespace google, and in some others,
// in gflags. This hack is enabling us to find both.
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

DEFINE_uint64(payload_size, 1, "Size of the payload to send");
DEFINE_string(target, "localhost:443", "Target host:port");
DEFINE_bool(secure, false, "Run with security?");
DEFINE_string(scenario, "ping-pong-request", "Scenario");
DEFINE_uint64(warmup, 1, "Warmup seconds");
DEFINE_uint64(benchmark, 5, "Benchmark seconds");

static gpr_histogram* histogram;
static grpc_byte_buffer* the_buffer;
static grpc_channel* channel;
static grpc_completion_queue* cq;
static grpc_call* call;
static grpc_op ops[6];
static grpc_op stream_init_ops[2];
static grpc_op stream_step_ops[2];
static grpc_metadata_array initial_metadata_recv;
static grpc_metadata_array trailing_metadata_recv;
static grpc_byte_buffer* response_payload_recv = nullptr;
static grpc_status_code status;
static grpc_slice details;
static grpc_op* op;

static void init_ping_pong_request(void) {
  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_metadata_array_init(&trailing_metadata_recv);

  memset(ops, 0, sizeof(ops));
  op = ops;

  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op++;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message.send_message = the_buffer;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &response_payload_recv;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op++;
}

static void step_ping_pong_request(void) {
  GPR_TIMER_BEGIN("ping_pong", 1);
  grpc_slice host = grpc_slice_from_static_string("localhost");
  call = grpc_channel_create_call(
      channel, nullptr, GRPC_PROPAGATE_DEFAULTS, cq,
      grpc_slice_from_static_string("/Reflector/reflectUnary"), &host,
      gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(call, ops,
                                                   (size_t)(op - ops), (void*)1,
                                                   nullptr));
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  grpc_call_unref(call);
  grpc_byte_buffer_destroy(response_payload_recv);
  call = nullptr;
  GPR_TIMER_END("ping_pong", 1);
}

static void init_ping_pong_stream(void) {
  grpc_metadata_array_init(&initial_metadata_recv);

  grpc_call_error error;
  grpc_slice host = grpc_slice_from_static_string("localhost");
  call = grpc_channel_create_call(
      channel, nullptr, GRPC_PROPAGATE_DEFAULTS, cq,
      grpc_slice_from_static_string("/Reflector/reflectStream"), &host,
      gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  stream_init_ops[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  stream_init_ops[0].data.send_initial_metadata.count = 0;
  stream_init_ops[1].op = GRPC_OP_RECV_INITIAL_METADATA;
  stream_init_ops[1].data.recv_initial_metadata.recv_initial_metadata =
      &initial_metadata_recv;
  error = grpc_call_start_batch(call, stream_init_ops, 2, (void*)1, nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);

  grpc_metadata_array_init(&initial_metadata_recv);

  stream_step_ops[0].op = GRPC_OP_SEND_MESSAGE;
  stream_step_ops[0].data.send_message.send_message = the_buffer;
  stream_step_ops[1].op = GRPC_OP_RECV_MESSAGE;
  stream_step_ops[1].data.recv_message.recv_message = &response_payload_recv;
}

static void step_ping_pong_stream(void) {
  grpc_call_error error;
  GPR_TIMER_BEGIN("ping_pong", 1);
  error = grpc_call_start_batch(call, stream_step_ops, 2, (void*)1, nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  grpc_byte_buffer_destroy(response_payload_recv);
  GPR_TIMER_END("ping_pong", 1);
}

static double now(void) {
  gpr_timespec tv = gpr_now(GPR_CLOCK_REALTIME);
  return 1e9 * (double)tv.tv_sec + tv.tv_nsec;
}

typedef struct {
  const char* name;
  void (*init)();
  void (*do_one_step)();
} scenario;

static const scenario scenarios[] = {
    {"ping-pong-request", init_ping_pong_request, step_ping_pong_request},
    {"ping-pong-stream", init_ping_pong_stream, step_ping_pong_stream},
};

int main(int argc, char** argv) {
  grpc_slice slice = grpc_slice_from_copied_string("x");
  double start, stop;
  unsigned i;

  char* fake_argv[1];

  grpc_event event;
  scenario sc = {nullptr, nullptr, nullptr};

  gpr_timers_set_log_filename("latency_trace.fling_client.txt");

  grpc_init();

  GPR_ASSERT(argc >= 1);
  fake_argv[0] = argv[0];
  grpc_test_init(1, fake_argv);

  ParseCommandLineFlags(&argc, &argv, true);

  for (i = 0; i < GPR_ARRAY_SIZE(scenarios); i++) {
    if (0 == strcmp(scenarios[i].name, FLAGS_scenario.c_str())) {
      sc = scenarios[i];
    }
  }
  if (!sc.name) {
    fprintf(stderr,
            "unsupported scenario '%s'. Valid are:", FLAGS_scenario.c_str());
    for (i = 0; i < GPR_ARRAY_SIZE(scenarios); i++) {
      fprintf(stderr, " %s", scenarios[i].name);
    }
    return 1;
  }

  channel =
      grpc_insecure_channel_create(FLAGS_target.c_str(), nullptr, nullptr);
  cq = grpc_completion_queue_create_for_next(nullptr);
  the_buffer = grpc_raw_byte_buffer_create(&slice, (size_t)FLAGS_payload_size);
  histogram = gpr_histogram_create(0.01, 60e9);

  sc.init();

  gpr_timespec end_warmup = grpc_timeout_seconds_to_deadline(FLAGS_warmup);
  gpr_timespec end_profiling =
      grpc_timeout_seconds_to_deadline(FLAGS_warmup + FLAGS_benchmark);

  while (gpr_time_cmp(gpr_now(end_warmup.clock_type), end_warmup) < 0) {
    sc.do_one_step();
  }

  gpr_log(GPR_INFO, "start profiling");
  grpc_profiler_start("client.prof");
  while (gpr_time_cmp(gpr_now(end_profiling.clock_type), end_profiling) < 0) {
    start = now();
    sc.do_one_step();
    stop = now();
    gpr_histogram_add(histogram, stop - start);
  }
  grpc_profiler_stop();

  if (call) {
    grpc_call_unref(call);
  }

  grpc_channel_destroy(channel);
  grpc_completion_queue_shutdown(cq);
  do {
    event = grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME),
                                       nullptr);
  } while (event.type != GRPC_QUEUE_SHUTDOWN);
  grpc_completion_queue_destroy(cq);
  grpc_byte_buffer_destroy(the_buffer);
  grpc_slice_unref(slice);

  gpr_log(GPR_INFO, "latency (50/95/99/99.9): %f/%f/%f/%f",
          gpr_histogram_percentile(histogram, 50),
          gpr_histogram_percentile(histogram, 95),
          gpr_histogram_percentile(histogram, 99),
          gpr_histogram_percentile(histogram, 99.9));
  gpr_histogram_destroy(histogram);

  grpc_shutdown();

  return 0;
}
