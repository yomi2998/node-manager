cmake_minimum_required(VERSION 4.2.3)
project(node-manager)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(node_test_password
    node_test_password.cpp
)

add_executable(node_test_sudoku
    node_test_sudoku.cpp
)