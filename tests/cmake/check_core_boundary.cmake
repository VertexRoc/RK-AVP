file(GLOB core_sources
  "${RKAVP_SOURCE_DIR}/src/core/*.cpp"
  "${RKAVP_SOURCE_DIR}/include/rkavp/*.hpp"
)
set(forbidden
  "rk_mpi.h"
  "im2d.hpp"
  "rknn_api.h"
  "alsa/asoundlib.h"
  "mk_mediakit.h"
  "CL/cl.h"
)
foreach(source IN LISTS core_sources)
  file(READ "${source}" contents)
  foreach(token IN LISTS forbidden)
    string(FIND "${contents}" "${token}" position)
    if(NOT position EQUAL -1)
      message(FATAL_ERROR "core architecture boundary violated by ${source}: ${token}")
    endif()
  endforeach()
endforeach()

foreach(umbrella core.hpp rkavp.hpp)
  file(READ "${RKAVP_SOURCE_DIR}/include/rkavp/${umbrella}" contents)
  foreach(module_header audio.hpp opencl.hpp rockchip.hpp streaming.hpp)
    string(FIND "${contents}" "rkavp/${module_header}" position)
    if(NOT position EQUAL -1)
      message(FATAL_ERROR "core umbrella ${umbrella} includes optional module: ${module_header}")
    endif()
  endforeach()
endforeach()

set(link_file "${RKAVP_BUILD_DIR}/CMakeFiles/rkavp_core.dir/link.txt")
if(EXISTS "${link_file}")
  file(READ "${link_file}" link_line)
  foreach(token mpp rga rknn asound mk_api OpenCL)
    string(FIND "${link_line}" "${token}" position)
    if(NOT position EQUAL -1)
      message(FATAL_ERROR "rkavp_core links forbidden dependency: ${token}")
    endif()
  endforeach()
endif()
