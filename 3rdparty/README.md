# Third-party dependencies

RK-AVP separates immutable source checkouts from generated build files:

```text
3rdparty/src/              pinned Git checkouts, ignored by Git
build/<preset>/_deps/      FetchContent build and subbuild trees
build/<preset>/3rdparty/   dependency targets with custom build directories
```

Run the prefetch target before an offline or repeatable build:

```bash
cmake --preset host-debug
cmake --build --preset host-debug --target rkavp-prefetch
```

`versions.cmake` is the source of truth for dependency revisions. Do not place
generated objects or CMake caches below `3rdparty/src`, and do not link files
from another preset's build directory.
