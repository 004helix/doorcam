project(
    'doorcam',
    'c',
    'cpp',
    version: '0.0',
    license: 'GPL',
    meson_version: '>=0.56.0',
    default_options: [
        'cpp_std=c++14',
        'c_std=c11',
    ],
)

# define OpenCL version
add_global_arguments('-DCL_TARGET_OPENCL_VERSION=300', language: 'cpp')

# ignore unknown pragmas
cxx = meson.get_compiler('cpp')
add_project_arguments(cxx.get_supported_arguments(['-Wno-unknown-pragmas']), language: 'cpp')

# qrscan dependencies
libavfilter = dependency('libavfilter')
libavcodec = dependency('libavcodec')
libavutil = dependency('libavutil')
opencv = dependency('opencv4', version:'>= 4.5')
opencl = dependency('OpenCL')
libva = dependency('libva')

dbr_lib = 'DynamsoftBarcodeReader'
dbr_lib_dir = meson.project_source_root() + '/lib'
dbr_include_dir = include_directories('include')
dbr = declare_dependency(
    link_args: ['-L' + dbr_lib_dir, '-l' + dbr_lib],
    include_directories: dbr_include_dir
)

# libqrscan.so
qrscan_dependencies = [
    libavfilter,
    libavcodec,
    libavutil,
    opencv,
    opencl,
    libva,
    dbr,
]

qrscan_sources = [
    'qrscan.cpp',
    'jpeg2umat.cpp'
]

shared_library(
    'qrscan',
    qrscan_sources,
    dependencies: qrscan_dependencies,
)

# libmotion.so
shared_library(
    'motion',
    ['motion.c'],
    c_args: '-msse4.2',
)

# libv4l2mjpg.so
shared_library(
    'v4l2mjpg',
    ['v4l2mjpg.c'],
)
