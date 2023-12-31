# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

add_custom_target(cls_arrow)
arrow_install_all_headers("arrow/adapters/arrow-rados-cls")

# cls_arrow
set(ARROW_SKYHOOK_SRCS cls_arrow.cc)

set(ARROW_SKYHOOK_LINK_STATIC arrow_dataset_static)
set(ARROW_SKYHOOK_LINK_SHARED arrow_dataset_shared)

find_package(librados REQUIRED)
include_directories(${LIBRADOS_INCLUDE_DIR})

add_arrow_lib(cls_arrow
              CMAKE_PACKAGE_NAME
              ArrowCls
              PKG_CONFIG_NAME
              arrow-cls
              OUTPUTS
              ARROW_SKYHOOK_LIBRARIES
              SOURCES
              ${ARROW_SKYHOOK_SRCS}
              PRIVATE_INCLUDES
              ${ARROW_SKYHOOK_PRIVATE_INCLUDES}
              SHARED_LINK_LIBS
              ${ARROW_SKYHOOK_LINK_SHARED}
              STATIC_LINK_LIBS
              ${ARROW_SKYHOOK_LINK_STATIC})

if(ARROW_TEST_LINKAGE STREQUAL "static")
  set(ARROW_DATASET_TEST_LINK_LIBS arrow_dataset_static ${ARROW_TEST_STATIC_LINK_LIBS})
else()
  set(ARROW_DATASET_TEST_LINK_LIBS arrow_dataset_shared ${ARROW_TEST_SHARED_LINK_LIBS})
endif()

foreach(LIB_TARGET ${ARROW_DATASET_LIBRARIES})
  target_compile_definitions(${LIB_TARGET} PRIVATE ARROW_DS_EXPORTING)
endforeach()

# cls_arrow_test
add_arrow_test(cls_arrow_test
               SOURCES
               cls_arrow_test.cc
               EXTRA_LINK_LIBS
               ${ARROW_DATASET_TEST_LINK_LIBS}
               PREFIX
               "arrow-cls")
