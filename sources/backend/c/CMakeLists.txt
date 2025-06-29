cmake_minimum_required(VERSION 3.25)
project(c C)

set(CMAKE_C_STANDARD 11)

find_package(MPI REQUIRED)
include_directories(${MPI_INCLUDE_PATH})

find_package(OpenMP REQUIRED)

# Erzeuge Executable (nur eine main()-haltige Datei)
add_executable(c_src fractal_C_openMP_live.c fractal_C_MPI_live.c fractal_C_MPI_live_CLIArgs.c)

# Verlinken mit dem Executable
target_link_libraries(c_src
        MPI::MPI_C
        OpenMP::OpenMP_C
        )
