cmake_minimum_required(VERSION 4.2)
project(EdgeOn_Client LANGUAGES CUDA CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# ── CUDA 툴킷 찾기 ───────────────────────────────────────────────
find_package(CUDAToolkit 12.9 EXACT REQUIRED )

# ── Qt 경로 분기 ────────────────────────────────────────────────
set(QT_ROOT_OVERRIDE "" CACHE PATH "Custom Qt root path (overrides auto-detection)")

if(QT_ROOT_OVERRIDE)
    set(_qt_root "${QT_ROOT_OVERRIDE}")
elseif(WIN32 AND MSVC)
    set(_qt_root "C:/Qt/6.11.0/msvc2022_64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_qt_root "/home/a1009mys/Qt/6.11.0/gcc_64")
endif()

if(_qt_root)
    list(PREPEND CMAKE_PREFIX_PATH "${_qt_root}")
    message(STATUS "Qt root: ${_qt_root}")
else()
    message(STATUS "Qt root: system path")
endif()

# ── OpenCV 경로 분기 ────────────────────────────────────────────
set(OpenCV_DIR "E:/library_source/opencv/opencv-4.13.0/build")
set(OpenCV_INCLUDE_DIRS "E:/library_source/opencv/opencv-4.13.0/build/include")
set(OpenCV_LIB_DIR "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/lib")
set(OpenCV_DLL_RELEASE "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/bin/opencv_world4130.dll")
set(OpenCV_DLL_DEBUG "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/bin/opencv_world4130d.dll")
set(OpenCV_FFMPEG_DLL "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/bin/opencv_videoio_ffmpeg4130_64.dll")

# ── 패키지 탐색 ─────────────────────────────────────────────────
find_package(Qt6 COMPONENTS Widgets Sql REQUIRED)

message(STATUS "OpenCV include : ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV lib dir : ${OpenCV_LIB_DIR}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ── 타겟 ────────────────────────────────────────────────────────
add_executable(EdgeOn_Client
    main.cpp
    streamworker.h   streamworker.cpp
    videocell.h      videocell.cpp
    mainwindow.h     mainwindow.cpp
)

target_link_libraries(EdgeOn_Client PRIVATE
    CUDA::cudart
    CUDA::nvml
    CUDA::cuda_driver
)

target_link_directories(EdgeOn_Client PRIVATE ${OpenCV_LIB_DIR})

target_link_libraries(EdgeOn_Client PRIVATE
    Qt6::Widgets
    Qt6::Sql
    opencv_world4130$<$<CONFIG:Debug>:d>
)

if(WIN32)
    target_link_libraries(EdgeOn_Client PRIVATE d2d1 dwrite d3d11 dxgi)
endif()

target_include_directories(EdgeOn_Client PRIVATE ${OpenCV_INCLUDE_DIRS})

if(WIN32)
    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    set(_windeployqt_executable "${_qt_bin_dir}/windeployqt.exe")

    set(_opencv_runtime_copy_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<$<CONFIG:Debug>:${OpenCV_DLL_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${OpenCV_DLL_RELEASE}>"
            "$<TARGET_FILE_DIR:EdgeOn_Client>"
    )

    # FFMPEG backend 플러그인 DLL은 opencv_world와 별개라서 실행 폴더에 별도 배포가 필요하다.
    if(EXISTS "${OpenCV_FFMPEG_DLL}")
        list(APPEND _opencv_runtime_copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OpenCV_FFMPEG_DLL}"
                "$<TARGET_FILE_DIR:EdgeOn_Client>"
        )
    endif()

    if(EXISTS "${_windeployqt_executable}")
        add_custom_command(TARGET EdgeOn_Client POST_BUILD
            ${_opencv_runtime_copy_commands}
            COMMAND "${_windeployqt_executable}"
                "$<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>"
                --no-translations
                --no-compiler-runtime
                --dir "$<TARGET_FILE_DIR:EdgeOn_Client>"
                "$<TARGET_FILE:EdgeOn_Client>"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found: ${_windeployqt_executable}")
    endif()
endif()

