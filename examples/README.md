# Framework examples

Examples demonstrate RK-AVP public APIs without introducing model-specific or
product-specific behavior.

- `hello_graph`: loads a YAML graph, observes an output stream, submits a
  timestamped Packet, closes the input stream, and waits for graph completion.

Build and run from the repository root:

```bash
cmake -S . -B build/examples -DRKAVP_BUILD_EXAMPLES=ON
cmake --build build/examples --parallel
build/examples/examples/rkavp-hello-graph graphs/hello_graph.yaml
```

Camera products, model postprocessing, Web APIs, and deployment-specific
applications belong in external repositories that consume installed
`RKAVP::*` CMake targets.
