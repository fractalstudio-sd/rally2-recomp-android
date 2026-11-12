cmake_minimum_required(VERSION 3.18)
project(srally2_android C CXX)

# SDL2 (downloaded by android/build.sh into app/jni/SDL2) builds libSDL2.so for us.
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/SDL2 SDL2-build)

set(M3_GLES ON CACHE BOOL "" FORCE)
if(NOT M3_REPO_ROOT)
  get_filename_component(M3_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()
add_subdirectory(${M3_REPO_ROOT} srally2-build)
target_link_libraries(srally2 PRIVATE SDL2::SDL2)
