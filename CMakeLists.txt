cmake_minimum_required(VERSION 4.2)
project(EdgeOn_Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

include(GNUInstallDirs)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT
   OR CMAKE_INSTALL_PREFIX STREQUAL "C:/Program Files/${PROJECT_NAME}"
   OR CMAKE_INSTALL_PREFIX STREQUAL "C:/Program Files (x86)/${PROJECT_NAME}")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path prefix." FORCE)
endif()

# ── CUDA 툴킷 찾기 (선택사항) ────────────────────────────────────
if(NOT DEFINED SKIP_CUDA)
    set(SKIP_CUDA FALSE)
endif()

if(NOT SKIP_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit 12.9 EXACT REQUIRED)
    message(STATUS "CUDA Toolkit found and enabled")
else()
    message(STATUS "CUDA Toolkit skipped (SKIP_CUDA=ON)")
endif()

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

set(FFMPEG_BIN_DIR "C:/ffmpeg/ffmpeg-master-latest-win64-gpl-shared/bin" CACHE PATH "Directory containing ffmpeg.exe and runtime DLLs")
set(FFMPEG_EXECUTABLE "" CACHE FILEPATH "Path to ffmpeg executable for recorder (optional, overrides FFMPEG_BIN_DIR)")
set(ONNXRUNTIME_ROOT "C:/onnxruntime/onnxruntime-win-x64-gpu-1.26.0" CACHE PATH "Path to ONNX Runtime root")

set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")

if(WIN32)
    if(NOT FFMPEG_EXECUTABLE AND FFMPEG_BIN_DIR)
        set(_ffmpeg_from_bin "${FFMPEG_BIN_DIR}/ffmpeg.exe")
        if(EXISTS "${_ffmpeg_from_bin}")
            set(FFMPEG_EXECUTABLE "${_ffmpeg_from_bin}")
        endif()
    endif()
endif()

# ── 패키지 탐색 ─────────────────────────────────────────────────
find_package(Qt6 COMPONENTS Widgets Sql Multimedia MultimediaWidgets REQUIRED)

message(STATUS "OpenCV include : ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV lib dir : ${OpenCV_LIB_DIR}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ── 타겟 ────────────────────────────────────────────────────────
add_executable(EdgeOn_Client
        src/main.cpp
        include/streamworker.h src/streamworker.cpp
        include/recorderworker.h src/recorderworker.cpp
        include/videocell.h src/videocell.cpp
        include/mainwindow.h src/mainwindow.cpp
        src/DBManager.cpp
        include/DBManager.h
        src/DBManager_cameras.cpp
        include/Protocol.h
        include/vodtimeline.h src/vodtimeline.cpp
        include/vodpanel.h src/vodpanel.cpp
        include/vodvideoview.h src/vodvideoview.cpp
        include/vodvideooverlay.h src/vodvideooverlay.cpp
        include/onnxinference.h src/onnxinference.cpp
        include/yolov8inference.h src/yolov8inference.cpp
)

set_target_properties(EdgeOn_Client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/$<CONFIG>/${CMAKE_INSTALL_BINDIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/$<CONFIG>/${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/$<CONFIG>/${CMAKE_INSTALL_LIBDIR}"
)

if(NOT SKIP_CUDA AND CUDAToolkit_FOUND)
    target_link_libraries(EdgeOn_Client PRIVATE
        CUDA::cudart
        CUDA::nvml
        CUDA::cuda_driver
    )
endif()

target_link_directories(EdgeOn_Client PRIVATE ${OpenCV_LIB_DIR})

if(EXISTS "${ONNXRUNTIME_LIB_DIR}")
    target_link_directories(EdgeOn_Client PRIVATE ${ONNXRUNTIME_LIB_DIR})
endif()

target_link_libraries(EdgeOn_Client PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    opencv_world4130$<$<CONFIG:Debug>:d>
    onnxruntime
)

if(WIN32)
    target_link_libraries(EdgeOn_Client PRIVATE d2d1 dwrite d3d11 dxgi)
endif()

target_include_directories(EdgeOn_Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

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

    file(GLOB _onnxruntime_lib_dlls "${ONNXRUNTIME_LIB_DIR}/*.dll")
    file(GLOB _onnxruntime_bin_dlls "${ONNXRUNTIME_ROOT}/bin/*.dll")
    set(_onnxruntime_runtime_dlls ${_onnxruntime_lib_dlls} ${_onnxruntime_bin_dlls})

    foreach(_onnxruntime_dll ${_onnxruntime_runtime_dlls})
        list(APPEND _opencv_runtime_copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_onnxruntime_dll}"
                "$<TARGET_FILE_DIR:EdgeOn_Client>"
        )
    endforeach()

    if(_onnxruntime_runtime_dlls)
        install(FILES ${_onnxruntime_runtime_dlls}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    else()
        message(WARNING "ONNX Runtime DLLs not found under '${ONNXRUNTIME_ROOT}/lib' or '${ONNXRUNTIME_ROOT}/bin'.")
    endif()

    if(FFMPEG_EXECUTABLE AND EXISTS "${FFMPEG_EXECUTABLE}")
        list(APPEND _opencv_runtime_copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_EXECUTABLE}"
                "$<TARGET_FILE_DIR:EdgeOn_Client>/ffmpeg${CMAKE_EXECUTABLE_SUFFIX}"
        )

        install(PROGRAMS "${FFMPEG_EXECUTABLE}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME "ffmpeg${CMAKE_EXECUTABLE_SUFFIX}"
        )

        get_filename_component(_ffmpeg_bin_dir "${FFMPEG_EXECUTABLE}" DIRECTORY)
        file(GLOB _ffmpeg_runtime_dlls "${_ffmpeg_bin_dir}/*.dll")
        foreach(_ffmpeg_dll ${_ffmpeg_runtime_dlls})
            list(APPEND _opencv_runtime_copy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_ffmpeg_dll}"
                    "$<TARGET_FILE_DIR:EdgeOn_Client>"
            )
        endforeach()

        if(_ffmpeg_runtime_dlls)
            install(FILES ${_ffmpeg_runtime_dlls}
                DESTINATION ${CMAKE_INSTALL_BINDIR}
            )
        endif()
    else()
        message(WARNING "ffmpeg executable not found. Set FFMPEG_EXECUTABLE or FFMPEG_BIN_DIR. Current FFMPEG_BIN_DIR='${FFMPEG_BIN_DIR}'")
    endif()

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

    # ── models 폴더 복사 ───────────────────────────────────────
    set(_models_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/models")
    if(EXISTS "${_models_source_dir}")
        add_custom_command(TARGET EdgeOn_Client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_models_source_dir}"
                "$<TARGET_FILE_DIR:EdgeOn_Client>/models"
            COMMENT "Copying models directory..."
        )

        install(DIRECTORY "${_models_source_dir}/"
            DESTINATION "${CMAKE_INSTALL_BINDIR}/models"
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
else()

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

