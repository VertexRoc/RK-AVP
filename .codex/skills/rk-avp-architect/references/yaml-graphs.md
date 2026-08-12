# YAML graph rules

- Keep `version: 1` at the root.
- Every node needs a unique `id` and registered `type`.
- Use `${NAME:-default}` for deploy-time values.
- Address ports as `node.port`.
- Every asynchronous edge declares `capacity` and `policy`.
- Use `drop_oldest` for latency-sensitive inference/video preview, `block` for lossless control/audio, and `drop_newest` only when preserving queued work is more important than fresh data.
- Validate type, port, caps, option, duplicate ID, and cycle errors before starting workers.
