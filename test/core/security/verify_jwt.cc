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

#include <stdio.h>
#include <string.h>

#include <gflags/gflags.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>

#include "src/core/lib/security/credentials/jwt/jwt_verifier.h"

// In some distros, gflags is in the namespace google, and in some others,
// in gflags. This hack is enabling us to find both.
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

DEFINE_string(jwt, "", "JSON web token to verify");
DEFINE_string(aud, "", "Audience for the JWT");

typedef struct {
  grpc_pollset* pollset;
  gpr_mu* mu;
  int is_done;
  int success;
} synchronizer;

static bool validate_flags() {
  if (FLAGS_jwt.empty() || FLAGS_aud.empty()) {
    return false;
  }
  return true;
}

static void on_jwt_verification_done(grpc_exec_ctx* exec_ctx, void* user_data,
                                     grpc_jwt_verifier_status status,
                                     grpc_jwt_claims* claims) {
  synchronizer* sync = static_cast<synchronizer*>(user_data);

  sync->success = (status == GRPC_JWT_VERIFIER_OK);
  if (sync->success) {
    char* claims_str;
    GPR_ASSERT(claims != nullptr);
    claims_str =
        grpc_json_dump_to_string((grpc_json*)grpc_jwt_claims_json(claims), 2);
    printf("Claims: \n\n%s\n", claims_str);
    gpr_free(claims_str);
    grpc_jwt_claims_destroy(exec_ctx, claims);
  } else {
    GPR_ASSERT(claims == nullptr);
    fprintf(stderr, "Verification failed with error %s\n",
            grpc_jwt_verifier_status_to_string(status));
  }

  gpr_mu_lock(sync->mu);
  sync->is_done = 1;
  GRPC_LOG_IF_ERROR("pollset_kick",
                    grpc_pollset_kick(exec_ctx, sync->pollset, nullptr));
  gpr_mu_unlock(sync->mu);
}

int main(int argc, char** argv) {
  synchronizer sync;
  grpc_jwt_verifier* verifier;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  grpc_init();
  ParseCommandLineFlags(&argc, &argv, true);

  if (!validate_flags()) {
    fprintf(stderr,
            "Missing or invalid arguments. Print help for more information\n");
    return 1;
  }

  verifier = grpc_jwt_verifier_create(nullptr, 0);

  grpc_init();

  sync.pollset = static_cast<grpc_pollset*>(gpr_zalloc(grpc_pollset_size()));
  grpc_pollset_init(sync.pollset, &sync.mu);
  sync.is_done = 0;

  grpc_jwt_verifier_verify(&exec_ctx, verifier, sync.pollset, FLAGS_jwt.c_str(),
                           FLAGS_aud.c_str(), on_jwt_verification_done, &sync);

  gpr_mu_lock(sync.mu);
  while (!sync.is_done) {
    grpc_pollset_worker* worker = nullptr;
    if (!GRPC_LOG_IF_ERROR("pollset_work",
                           grpc_pollset_work(&exec_ctx, sync.pollset, &worker,
                                             GRPC_MILLIS_INF_FUTURE)))
      sync.is_done = true;
    gpr_mu_unlock(sync.mu);
    grpc_exec_ctx_flush(&exec_ctx);
    gpr_mu_lock(sync.mu);
  }
  gpr_mu_unlock(sync.mu);

  gpr_free(sync.pollset);

  grpc_jwt_verifier_destroy(&exec_ctx, verifier);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
  return !sync.success;
}
