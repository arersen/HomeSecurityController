cmake_minimum_required(VERSION 3.10)
project(HomeSecurityController C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # gnu11: clock_gettime, sigaction тощо

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror=implicit-function-declaration")

include_directories(include)

set(SOURCES
		src/main.c
		src/core/logger.c
		src/core/events.c
		src/core/card_store.c
		src/core/watchdog.c
		src/gpio/buzzer.c
		src/i2c/i2c.c
		src/i2c/sht3x.c
		src/spi/rc522.c
		src/gpio/gpio.c
		src/gpio/radar.c
		src/etc.c
		src/board_config.c
		src/gpio/traffic_light.c
)

add_executable(security_daemon ${SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(security_daemon PRIVATE Threads::Threads gpiod)

install(TARGETS security_daemon RUNTIME DESTINATION bin)
install(PROGRAMS init/S99security-daemon DESTINATION /etc/init.d)

# ---------------- Деплой на плату (SSH) ----------------
# Уся логіка — у scripts/deploy.sh (його можна запускати і вручну).
set(DEPLOY_USER "root" CACHE STRING "SSH user deploy")
set(DEPLOY_HOST "192.168.137.127" CACHE STRING "IP deploy")
option(DEPLOY_LIBGPIOD "Копіювати libgpiod із sysroot Buildroot у /usr/lib на платі" ON)

set(DEPLOY_LIB "")
if(DEPLOY_LIBGPIOD)
	find_library(GPIOD_LIBRARY gpiod)
	if(GPIOD_LIBRARY)
		get_filename_component(DEPLOY_LIB "${GPIOD_LIBRARY}" REALPATH)
	endif()
endif()

set(DEPLOY_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/deploy.sh)
set(DEPLOY_TARGET ${DEPLOY_USER}@${DEPLOY_HOST})

# зібрати + встановити як сервіс + синхронізувати час + запустити сервіс
add_custom_target(deploy
		COMMAND sh ${DEPLOY_SCRIPT} ${DEPLOY_TARGET} $<TARGET_FILE:security_daemon> ${DEPLOY_LIB}
		DEPENDS security_daemon
		COMMENT "Deploying security_daemon to ${DEPLOY_TARGET} (service)"
		VERBATIM
)

# те саме, але сервіс не стартує: демон запускається тут, у терміналі (налагодження)
add_custom_target(run
		COMMAND sh ${DEPLOY_SCRIPT} --run ${DEPLOY_TARGET} $<TARGET_FILE:security_daemon> ${DEPLOY_LIB}
		DEPENDS security_daemon
		USES_TERMINAL
		COMMENT "Running security_daemon on ${DEPLOY_TARGET} (foreground)"
		VERBATIM
)

# журнал подій сервісу в реальному часі
add_custom_target(logs
		COMMAND ssh -tt -o ConnectTimeout=20 ${DEPLOY_TARGET} "tail -n 30 -f /var/lib/security_daemon/security.log"
		USES_TERMINAL
		VERBATIM
)
