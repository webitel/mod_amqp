cmake_minimum_required(VERSION 3.3)

project(mod_amqp C)

set(FREESWITCH_INCLUDE_DIR "/usr/local/freeswitch/include/freeswitch" CACHE PATH "Location of FreeSWITCH headers")
set(INSTALL_MOD_DIR "/usr/local/freeswitch/mod" CACHE PATH "Location install library")

add_library(mod_amqp SHARED mod_amqp_utils.c mod_amqp_connection.c mod_amqp_producer.c mod_amqp_command.c mod_amqp_logging.c mod_amqp_cdr_utils.c mod_amqp_cdr.c mod_amqp.c)
target_include_directories(mod_amqp PRIVATE  ${FREESWITCH_INCLUDE_DIR} )

target_link_libraries(mod_amqp PRIVATE -lrabbitmq)

set_target_properties(mod_amqp PROPERTIES PREFIX "")
set_target_properties(mod_amqp PROPERTIES OUTPUT_NAME "mod_amqp")

install(TARGETS mod_amqp DESTINATION ${INSTALL_MOD_DIR})