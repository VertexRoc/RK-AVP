# Docker and CI

- Use multi-stage Docker builds and keep compilers out of runtime images.
- Pass video, RGA, NPU, and audio devices explicitly at deployment time.
- Host CI runs unit, integration, ASan/UBSan, Python, and Docker jobs.
- Cross builds compile the core and Rockchip plugin for rk3588 and rk3576.
- Runtime images contain framework plugins and vendor runtime libraries, but no models or business graphs.
- Self-hosted board runners execute hardware and endurance labels.
- Never claim hardware acceleration was tested when only fake backends ran.
