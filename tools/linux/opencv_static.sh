#!/bin/bash
# OpenCV for the Linux package: static, and only what Bedrock uses.
#
# Ubuntu's OpenCV is built for everybody - its imgcodecs alone pull in GDAL,
# HDF5, Poppler, MySQL - and the package that carried it was 116 MB, most of
# it never called. Bedrock reads PNG and JPEG, does image processing, and
# runs one ONNX model through dnn. That is core, imgproc, imgcodecs and dnn,
# with PNG, JPEG, zlib and protobuf from OpenCV's own 3rdparty tree, and it
# is what the Windows build has too (vcpkg, no OpenCL, no CUDA).
#
# Run inside the jammy root (jammy_run.sh); lands in /opt/opencv-static.
set -e
VERSION=${OPENCV_VERSION:-4.10.0}
PREFIX=${OPENCV_PREFIX:-/opt/opencv-static}
WORK=${1:-/work/opencv}
mkdir -p "$WORK" && cd "$WORK"
if [ ! -d "opencv-$VERSION" ]; then
    curl -sSL -o "opencv-$VERSION.tar.gz" "https://github.com/opencv/opencv/archive/refs/tags/$VERSION.tar.gz"
    tar xzf "opencv-$VERSION.tar.gz"
fi
cmake -S "opencv-$VERSION" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_LIST=core,imgproc,imgcodecs,dnn \
    -DBUILD_ZLIB=ON -DBUILD_PNG=ON -DBUILD_JPEG=ON -DBUILD_PROTOBUF=ON \
    -DWITH_PNG=ON -DWITH_JPEG=ON -DWITH_PROTOBUF=ON \
    -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_OPENEXR=OFF \
    -DWITH_IMGCODEC_HDR=OFF -DWITH_IMGCODEC_SUNRASTER=OFF -DWITH_IMGCODEC_PXM=OFF -DWITH_IMGCODEC_PFM=OFF \
    -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_V4L=OFF -DWITH_GTK=OFF -DWITH_QT=OFF \
    -DWITH_OPENCL=OFF -DWITH_CUDA=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_TBB=OFF \
    -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF -DWITH_OPENVINO=OFF -DWITH_VULKAN=OFF -DWITH_VA=OFF \
    -DWITH_QUIRC=OFF -DWITH_ADE=OFF -DWITH_FLATBUFFERS=OFF -DWITH_OBSENSOR=OFF \
    -DOPENCV_DNN_OPENCL=OFF -DOPENCV_DNN_CUDA=OFF \
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF \
    -DBUILD_DOCS=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF \
    -DCPU_BASELINE=SSE4_2 -DCPU_DISPATCH=AVX,AVX2 \
    -DOPENCV_GENERATE_PKGCONFIG=OFF \
    > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
cmake --build build -j "$(nproc)" > build.log 2>&1 || { grep -n "error" build.log | head; exit 1; }
cmake --install build > install.log 2>&1
ls "$PREFIX/lib"
