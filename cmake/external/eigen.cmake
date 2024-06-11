if (onnxruntime_USE_PREINSTALLED_EIGEN)
    add_library(eigen INTERFACE)
    file(TO_CMAKE_PATH ${eigen_SOURCE_PATH} eigen_INCLUDE_DIRS)
    target_include_directories(eigen INTERFACE ${eigen_INCLUDE_DIRS})
else ()
    FetchContent_Declare(
        eigen
        URL https://gitlab.com/libeigen/eigen/-/archive/bb51d9f4fa3cf1114348b9180640d6da7d3964f9/eigen-bb51d9f4fa3cf1114348b9180640d6da7d3964f9.zip
        #URL_HASH SHA1=${DEP_SHA1_eigen}
    )

    FetchContent_Populate(eigen)
    set(eigen_INCLUDE_DIRS  "${eigen_SOURCE_DIR}")
endif()
