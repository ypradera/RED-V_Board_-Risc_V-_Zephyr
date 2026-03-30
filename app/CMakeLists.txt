cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(redv_app)

target_sources(app PRIVATE
    src/main.c
    src/bno085_task.c
    lib/sh2/sh2.c
    lib/sh2/shtp.c
    lib/sh2/sh2_SensorValue.c
    lib/sh2/sh2_util.c
    lib/sh2/euler.c
)

target_include_directories(app PRIVATE
    lib/sh2
    src
)
