cmake_minimum_required(VERSION 3.10)
project(HomeSecurityController C CXX)

set(CMAKE_C_STANDART 11)
set(CMAKE_C_STANDART_REQUIRED True)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror=implicit-function-declaration")

include_directories(include)

set(SOURCES
	src/main.c
	src/core/logger.c
	src/core/events.c
	src/i2c/i2c.c
	src/i2c/bme680.c
	src/spi/spi.c
	src/spi/rc522.c
	src/gpio/radar.c
)

add_executable(security_daemon ${SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(security_daemon PRIVATE Threads::Threads)


