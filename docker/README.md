# Docker

The root `Dockerfile` is the single image definition. The `docker/` directory
keeps one user-facing command and small Compose overlays; applications provide
their own graphs, plugins, models, and resources.

```text
docker/
  rkavp-docker         build and run entry point
  compose.yaml         production defaults
  compose.cdi.yaml     Rockchip CDI devices
  compose.debug.yaml   privileged diagnostics
```

## Quick start

```bash
docker/rkavp-docker build rk3588 rkavp:rk3588
RKAVP_IMAGE=rkavp:rk3588 docker/rkavp-docker run rk3588 doctor
```

The repository examples are mounted by default. Point `RKAVP_GRAPH_DIR` at an
application repository to run product graphs:

```bash
RKAVP_IMAGE=rkavp:rk3588 RKAVP_GRAPH_DIR=/opt/my-app/graphs \
  docker/rkavp-docker run rk3588 \
  run --graph /work/graphs/graph.yaml
```

The run command automatically uses an installed RK-AVP CDI specification. If
CDI is unavailable, it discovers existing camera, Media Controller, RGA, MPP,
RKNN, DRM, DMA heap, and ALSA devices and creates a temporary least-privilege
Compose override. Camera selection remains in the graph, not in the image.

Use privileged mode only for diagnostics:

```bash
docker/rkavp-docker run rk3588 --debug doctor
```

Force CDI when testing a generated specification:

```bash
sudo rkavp-ctk cdi generate --output /var/run/cdi
docker/rkavp-docker run rk3588 --cdi doctor
```

`runtime-rk3588/rk3576` images contain the framework and hardware plugins.
`devel-rk3588/rk3576` additionally contain headers and the installed CMake
package. `ci` and `sdk-arm64` serve host validation and plugin development.

Validate deployment configuration with:

```bash
docker/rkavp-docker validate
```
