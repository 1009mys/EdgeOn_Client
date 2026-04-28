cmake_minimum_required(VERSION 4.2)
project(EdgeOn_Client LANGUAGES CUDA CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

include(GNUInstallDirs)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT
   OR CMAKE_INSTALL_PREFIX STREQUAL "C:/Program Files/${PROJECT_NAME}"
   OR CMAKE_INSTALL_PREFIX STREQUAL "C:/Program Files (x86)/${PROJECT_NAME}")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path prefix." FORCE)
endif()

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
if (WIN32)
    set(OpenCV_DIR "E:/library_source/opencv/opencv-4.13.0/build")
    set(OpenCV_INCLUDE_DIRS "E:/library_source/opencv/opencv-4.13.0/build/include")
    set(OpenCV_LIB_DIR "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/lib")
    set(OpenCV_DLL_RELEASE "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/bin/opencv_world4130.dll")
    set(OpenCV_DLL_DEBUG "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/bin/opencv_world4130d.dll")
    set(OpenCV_FFMPEG_DLL "E:/library_source/opencv/opencv-4.13.0/build/x64/vc17/bin/opencv_videoio_ffmpeg4130_64.dll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
endif ()

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

set_target_properties(EdgeOn_Client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/$<CONFIG>/${CMAKE_INSTALL_BINDIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/$<CONFIG>/${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/$<CONFIG>/${CMAKE_INSTALL_LIBDIR}"
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

install(TARGETS EdgeOn_Client
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    BUNDLE DESTINATION .
)

if(WIN32)
    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    set(_windeployqt_executable "${_qt_bin_dir}/windeployqt.exe")

    get_target_property(_edgeon_output_name EdgeOn_Client OUTPUT_NAME)
    if(NOT _edgeon_output_name)
        set(_edgeon_output_name "EdgeOn_Client")
    endif()
    set(EDGEON_EXECUTABLE_NAME "${_edgeon_output_name}${CMAKE_EXECUTABLE_SUFFIX}")

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

        install(FILES "${OpenCV_FFMPEG_DLL}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

    if(EXISTS "${OpenCV_DLL_DEBUG}")
        install(FILES "${OpenCV_DLL_DEBUG}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            CONFIGURATIONS Debug
        )
    endif()

    if(EXISTS "${OpenCV_DLL_RELEASE}")
        install(FILES "${OpenCV_DLL_RELEASE}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            CONFIGURATIONS Release RelWithDebInfo MinSizeRel
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

        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployQt.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/DeployQt.cmake"
            @ONLY
        )
        install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployQt.cmake")
    else()
        message(WARNING "windeployqt not found: ${_windeployqt_executable}")
    endif()
endif()

if(NOT PROJECT_VERSION)
    set(_edgeon_package_version "0.1.0")
else()
    set(_edgeon_package_version "${PROJECT_VERSION}")
endif()

set(CPACK_PACKAGE_NAME "EdgeOn_Client")
set(CPACK_PACKAGE_VENDOR "EdgeOn")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "EdgeOn multi-channel RTSP client")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "EdgeOn_Client")
set(CPACK_PACKAGE_VERSION "${_edgeon_package_version}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "EdgeOn Client")
    set(CPACK_NSIS_PACKAGE_NAME "EdgeOn Client")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()

include(CPack)

