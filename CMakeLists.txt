cmake_minimum_required(VERSION 3.16)
project(multi_rec)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g2 -O2 -fPIC -pthread -pie -DARMv8 -D_USE_NEON -DUSE_TFACC40T")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(multi_rec src/multi_rec.cpp)

target_link_libraries(
    multi_rec 
    tfg
    tfdec 
    tfenc
    avcodec 
    avformat 
    avutil 
)