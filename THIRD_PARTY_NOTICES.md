# Third-Party Notices

RK-AVP can build and redistribute optional components from the projects below.
Exact source revisions are pinned in `3rdparty/versions.cmake`.

| Component | Upstream | Purpose | License source |
| --- | --- | --- | --- |
| yaml-cpp | https://github.com/jbeder/yaml-cpp | YAML graph configuration | Upstream repository |
| GoogleTest | https://github.com/google/googletest | Host tests only | Upstream repository |
| pybind11 | https://github.com/pybind/pybind11 | Optional Python bindings | Upstream repository |
| Rockchip MPP | https://github.com/rockchip-linux/mpp | Hardware video codec plugin | Copyright and license headers in upstream sources |
| Rockchip librga | https://github.com/airockchip/librga | 2D acceleration plugin | `librga-COPYING` in installed notices |
| RKNN Runtime | https://github.com/airockchip/rknn-toolkit2 | NPU inference plugin | Terms distributed by the upstream runtime package |
| ZLMediaKit | https://github.com/ZLMediaKit/ZLMediaKit | Streaming plugin | `ZLMediaKit-LICENSE` in installed notices |

The runtime container contains only dependencies enabled by its build target.
Consumers remain responsible for reviewing upstream terms for their selected
configuration and distribution territory.
