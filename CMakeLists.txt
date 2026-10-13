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
		src/gpio/buzzer.c
		src/i2c/i2c.c
		src/i2c/bme680.c
		src/spi/spi.c
		src/spi/rc522.c
		src/gpio/gpio.c
		src/gpio/radar.c

)

add_executable(security_daemon ${SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(security_daemon PRIVATE Threads::Threads gpiod)

set(DEPLOY_USER "root" CACHE STRING "SSH user deploy")
set(DEPLOY_HOST "192.168.0.184" CACHE STRING "IP deploy")
set(DEPLOY_PATH "/root" CACHE STRING "Deploy path")

#[[
add_custom_target(deploy
		COMMAND scp -O -o BatchMode=yes -o ConnectTimeout=5
		$<TARGET_FILE:security_daemon>
		${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}/
		DEPENDS security_daemon
		COMMENT "Deploying security_daemon to ${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}"
		VERBATIM
)
]]

set(SSH_OPTS -o BatchMode=yes -o ConnectTimeout=5)

add_custom_target(deploy
		COMMAND ssh ${SSH_OPTS} ${DEPLOY_USER}@${DEPLOY_HOST} "killall security_daemon 2>/dev/null || true"
		COMMAND scp -O ${SSH_OPTS}
		$<TARGET_FILE:security_daemon>
		${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}/
		DEPENDS security_daemon
		COMMENT "Deploying security_daemon to ${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}"
		VERBATIM
)

add_custom_target(run
		COMMAND ssh -tt ${SSH_OPTS} ${DEPLOY_USER}@${DEPLOY_HOST} ${DEPLOY_PATH}/security_daemon
		DEPENDS deploy
		USES_TERMINAL
		COMMENT "Running security_daemon on ${DEPLOY_USER}@${DEPLOY_HOST}"
		VERBATIM
)

