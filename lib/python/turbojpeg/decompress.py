from ctypes.util import find_library
import ctypes as ct
import os


class TJDecompress():
    # pixel formats
    TJPF_RGB = 0
    TJPF_BGR = 1
    TJPF_RGBX = 2
    TJPF_BGRX = 3
    TJPF_XBGR = 4
    TJPF_XRGB = 5
    TJPF_GRAY = 6
    TJPF_RGBA = 7
    TJPF_BGRA = 8
    TJPF_ABGR = 9
    TJPF_ARGB = 10

    # chrominance subsampling options
    TJSAMP_444 = 0
    TJSAMP_422 = 1
    TJSAMP_420 = 2
    TJSAMP_GRAY = 3
    TJSAMP_440 = 4

    # pixel size [pixel format]
    PIXEL_SIZE = (3, 3, 4, 4, 4, 4, 1, 4, 4, 4, 4)

    def __init__(self):
        turbo_jpeg = ct.cdll.LoadLibrary(find_library('turbojpeg'))

        self.__init_decompress = turbo_jpeg.tjInitDecompress
        self.__init_decompress.restype = ct.c_void_p

        self.__destroy = turbo_jpeg.tjDestroy
        self.__destroy.argtypes = [ct.c_void_p]
        self.__destroy.restype = ct.c_int

        self.__decompress = turbo_jpeg.tjDecompress2
        self.__decompress.argtypes = [
            ct.c_void_p, ct.POINTER(ct.c_ubyte), ct.c_ulong,
            ct.POINTER(ct.c_ubyte), ct.c_int, ct.c_int, ct.c_int,
            ct.c_int, ct.c_int
        ]
        self.__decompress.restype = ct.c_int

        self.handle = self.__init_decompress()

    def __del__(self):
        self.__destroy(self.handle)

    # raw c interface
    def decompress(self, src, src_size, dst,
                   width, height, pixel_format=TJPF_BGR, flags=0):
        src_addr = ct.cast(src, ct.POINTER(ct.c_ubyte))
        dst_addr = ct.cast(dst, ct.POINTER(ct.c_ubyte))

        status = self.__decompress(
            self.handle,
            src_addr, src_size, dst_addr,
            width, 0, height, pixel_format, flags
        )

        if status != 0:
            raise IOError('Failed to decompress')
