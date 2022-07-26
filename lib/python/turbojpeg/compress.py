from ctypes.util import find_library
import ctypes as ct
import os


class TJCompress():
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
        lib = ct.cdll.LoadLibrary(find_library('turbojpeg'))

        self.__init_compress = lib.tjInitCompress
        self.__init_compress.restype = ct.c_void_p

        self.__destroy = lib.tjDestroy
        self.__destroy.argtypes = [ct.c_void_p]
        self.__destroy.restype = ct.c_int

        self.__compress = lib.tjCompress2
        self.__compress.argtypes = [
            ct.c_void_p, ct.POINTER(ct.c_ubyte), ct.c_int, ct.c_int, ct.c_int,
            ct.c_int, ct.POINTER(ct.c_void_p), ct.POINTER(ct.c_ulong),
            ct.c_int, ct.c_int, ct.c_int
        ]
        self.__compress.restype = ct.c_int

        self.__free = lib.tjFree
        self.__free.argtypes = [ct.c_void_p]
        self.__free.restype = None

        self.handle = self.__init_compress()

    def __del__(self):
        self.__destroy(self.handle)

    def compress(self, src, src_size, w, h,
                 quality=85, pixel_format=TJPF_BGR,
                 jpeg_subsample=TJSAMP_422, flags=0):
        src_addr = ct.cast(src, ct.POINTER(ct.c_ubyte))

        jpeg_buf = ct.c_void_p()
        jpeg_size = ct.c_ulong()

        status = self.__compress(
            self.handle,
            src_addr, w, PIXEL_SIZE[pixel_format] * w, h, pixel_format,
            ct.byref(jpeg_buf), ct.byref(jpeg_size),
            jpeg_subsample, quality, flags
        )

        if status != 0:
            raise IOError('Failed to compress')

        dst = ct.string_at(jpeg_buf.value, jpeg_size.value)

        self.__free(jpeg_buf)

        return dst
