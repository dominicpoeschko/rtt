cmake_minimum_required(VERSION 3.28)

add_library(rtt INTERFACE)

target_include_directories(rtt INTERFACE src)

add_library(rtt::rtt ALIAS rtt)
