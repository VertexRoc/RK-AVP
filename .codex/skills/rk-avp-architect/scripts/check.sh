#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${repo_root}/build/skill-check"

cmake -S "${repo_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRKAVP_BUILD_TESTS=ON \
  -DRKAVP_BUILD_EXAMPLES=ON \
  -DRKAVP_FETCH_DEPS=OFF
cmake --build "${build_dir}" --parallel
"${build_dir}/examples/rkavp-hello-graph" "${repo_root}/graphs/hello_graph.yaml"
ctest --test-dir "${build_dir}" --output-on-failure
