cmake_minimum_required(VERSION 2.8)
project(pubsub)

set (CMAKE_CXX_STANDARD 17)
set(BOOST_ROOT /opt/version/3rd/boost)
set(PYTHON_ROOT /opt/anaconda3)
set(CMAKE_BUILD_TYPE "Release")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -fPIC -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -fPIC -Wall")

file(READ /inc/version.inc COMPILE_VERSION LIMIT 8 OFFSET 33)
string(STRIP ${COMPILE_VERSION} COMPILE_VERSION)
set(COMPILE_PATH /opt/version/${COMPILE_VERSION})
message(${COMPILE_PATH})

#message(${COMPILE_PATH1})
set(3RD_PATH /opt/version/3rd)

#include
set(SYSTEM_INC_DIR      ${COMPILE_PATH}/include)
set(BOOST_INCLUDE_DIR   ${3RD_PATH}/boost/include)
set(PRINTF_INCLUDE_DIR       ${SYSTEM_INC_DIR}/printf)
set(BOOST_PYTHON     ${PYTHON_ROOT}/include/python3.8)
set(SHMMAP_LIBRARY ${SYSTEM_INC_DIR}/pubsub)

set(SHMQUEUE_INCLUDE_DIR     ${SYSTEM_INC_DIR}/shmqueue)
set(FMT_INCLUDE_DIR          ${3RD_PATH}/fmt/include)
#set(SHMQUEUE_LIBRARY ${SYSTEM_INC_DIR}/shmqueue)
set(NANOMSG_INCLUDE_DIR       ${3RD_PATH}/nanomsg/include)

#link
set(BOOST_LIBRARY     ${3RD_PATH}/boost/lib)
set(FMT_LIBRARY        ${3RD_PATH}/fmt/lib64)
set(NANOMSG_LIBRARY     ${3RD_PATH}/nanomsg/lib)

#指定动态库的生成路径
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${COMPILE_PATH}/python-libs)
aux_source_directory(${SHMQUEUE_INCLUDE_DIR}  DIR_LIB_SRCS_SHMQUEUE)

FIND_PACKAGE(Boost COMPONENTS python)
FIND_PACKAGE(PythonInterp)
FIND_PACKAGE(PythonLibs)
FIND_PACKAGE(Boost COMPONENTS python38 REQUIRED)

add_subdirectory(pubsub)

include_directories(
        ${3RD_PATH}
        ${SYSTEM_INC_DIR}
        ${SHMMAP_LIBRARY}
        ${SHMQUEUE_INCLUDE_DIR}
        ${BOOST_INCLUDE_DIR}
        ${COMPILE_VERSION}
        ${PRINTF_INCLUDE_DIR}
        ${BOOST_PYTHON}
        ${FMT_INCLUDE_DIR}
	${NANOMSG_INCLUDE_DIR}
)

link_directories(
        ${LINK_DIR}
        ${BOOST_LIBRARY}
        /usr/local/lib64
        ${FMT_LIBRARY}
	${NANOMSG_LIBRARY}
)

link_libraries(
    boost_python38  pthread boost_system nanomsg
)

#PYTHON_ADD_MODULE(pubsub  src/pubsub.cpp  ${SHMMAP_LIBRARY}/m_pool.c ${PRINTF_INCLUDE_DIR}/printf.c)
#PYTHON_ADD_MODULE(test_pubsub pubsub/src/test_pubsub.cpp)
#PYTHON_ADD_MODULE(crypto_enum pubsub/src/crypto_enum.cpp)
PYTHON_ADD_MODULE(data_struct src/data_struct_enum.cpp)
