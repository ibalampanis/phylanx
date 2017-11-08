# Copyright (c) 2017 Parsa Amini
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

# setup Blaze as a dependency
macro(phylanx_setup_blaze)
  find_package(LAPACK REQUIRED)

  include_directories(${BLAS_INCLUDE_DIR})

  find_package(blaze NO_CMAKE_PACKAGE_REGISTRY)
  if(NOT blaze_FOUND)
    phylanx_error("Blaze could not be found. Please specify blaze_DIR to assist locating it.")
  endif()
endmacro()