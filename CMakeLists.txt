cmake_minimum_required(VERSION 2.8)
project(sys_include)

set (CMAKE_CXX_STANDARD 17)
set(BOOST_ROOT /opt/version/3rd/boost)
set(PYTHON_ROOT /opt/anaconda3)


file(READ /inc/version.inc VERSION LIMIT 8 OFFSET 8)
string(STRIP ${VERSION} VERSION)
set(COMPILE_PATH /opt/version/${VERSION}  )
set(COMPILE_PATH1 /opt/version/${VERSION} CACHE INTERNAL "COMPILE PATH")

set(3RD_PATH /opt/version/3rd)
#include

set(SYSTEM_INC_DIR           ${COMPILE_PATH}/include)
#set(SHMC_INC_DIR /opt/version/include/shm_container/src)
set(BOOST_INCLUDE_DIR        ${3RD_PATH}/boost/include)
set(CPPRESTSDK_INCLUDE_DIR   ${3RD_PATH}/cpprest/include)
set(CPP_REDIS_INCLUDE_DIR    ${3RD_PATH}/cpp_redis/includes)
set(TACOPIE_INCLUDE_DIR      ${3RD_PATH}/cpp_redis/tacopie/includes)
set(CPPZMQ_INCLUDE_DIR       ${3RD_PATH}/cppzmq)
set(ZEROMQ_INCLUDE_DIR       ${3RD_PATH}/zeromq/include)
set(LOCAL_INCLUDE_DIR        /usr/local/include)
set(SHMMAP_INCLUDE_DIR       ${SYSTEM_INC_DIR}/shmmap)
set(PRINTF_INCLUDE_DIR       ${SYSTEM_INC_DIR}/printf)
set(SHMQUEUE_INCLUDE_DIR     ${SYSTEM_INC_DIR}/shmqueue)
set(LIBGO_INCLUDE_DIR        ${3RD_PATH}/libgo/include/libgo)
#set(FMT_INCLUDE_DIR          ${SYSTEM_INC_DIR}/fmt/include)
set(ONETBB_INCLUDE_DIR       ${3RD_PATH}/oneTBB/include)

include_directories(
        ${3RD_PATH}
        ${INC_DIR} 
        ${SYSTEM_INC_DIR}
        ${SHMC_INC_DIR}
        ${BOOST_INCLUDE_DIR}
        ${CPPRESTSDK_INCLUDE_DIR}
        ${COMPILE_VERSION}
        ${CPP_REDIS_INCLUDE_DIR}
        ${TACOPIE_INCLUDE_DIR}
        ${CPPZMQ_INCLUDE_DIR}
        ${ZEROMQ_INCLUDE_DIR}
        ${LOCAL_INCLUDE_DIR}
        ${SHMMAP_INCLUDE_DIR}
        ${PRINTF_INCLUDE_DIR}
        ${SHMQUEUE_INCLUDE_DIR} 
        ${ONETBB_INCLUDE_DIR}
)
