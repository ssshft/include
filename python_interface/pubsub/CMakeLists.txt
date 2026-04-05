cmake_minimum_required(VERSION 2.8)
project(pubsub)

#message(${DIR_LIB_SRCS_SHMQUEUE})

#FIND_PACKAGE(Boost COMPONENTS python)
#FIND_PACKAGE(PythonInterp)
#FIND_PACKAGE(PythonLibs)
#FIND_PACKAGE(Boost COMPONENTS python38 REQUIRED)

include_directories(
        ${3RD_PATH}
        ${SYSTEM_INC_DIR}
        ${SHMQUEUE_INCLUDE_DIR}
        ${BOOST_INCLUDE_DIR}
        ${COMPILE_VERSION}
#        ${PRINTF_INCLUDE_DIR}
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
        boost_python38 pthread boost_system nanomsg
)

#PYTHON_ADD_MODULE(pubsub  src/pubsub.cpp  ${SHMMAP_LIBRARY}/m_pool.c ${PRINTF_INCLUDE_DIR}/printf.c)
# PYTHON_ADD_MODULE(test_pubsub src/test_pubsub.cpp)
PYTHON_ADD_MODULE(pypubsub
        src/pubsub.cpp
        ${SHMQUEUE_INCLUDE_DIR}/shmmqueue.cpp
        ${SHMQUEUE_INCLUDE_DIR}/shm_rwlock.cpp
#        ${SHMMAP_LIBRARY}/m_pool.c
#        ${PRINTF_INCLUDE_DIR}/printf.c
#        ${DIR_LIB_SRCS_SHMQUEUE}
        )
#PYTHON_ADD_MODULE(data_struct src/data_struct_enum.cpp)




