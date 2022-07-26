import ctypes as ct
import os.path


ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..', '..', '..'
))


class Motion:

    def __init__(self):
        libpath = os.path.join(ROOT, 'lib', 'libmotion.so')
        libmotion = ct.cdll.LoadLibrary(libpath)

        self.__cdb = libmotion.count_different_bytes
        self.__cdb.argtypes = [
            ct.POINTER(ct.c_ubyte), ct.POINTER(ct.c_ubyte),
            ct.c_ulong, ct.c_ubyte
        ]
        self.__cdb.restype = ct.c_long

    def count_different_bytes(self, a1, a2, size, threshold):
        res = self.__cdb(
            ct.cast(a1, ct.POINTER(ct.c_ubyte)),
            ct.cast(a2, ct.POINTER(ct.c_ubyte)),
            size,
            threshold
        )

        if res == -1:
            Exception('a1/a2 or size not alligned to 16 bytes')

        return res
