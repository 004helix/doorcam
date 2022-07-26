import ctypes as ct
import os.path


ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..', '..', '..'
))


class V4L2MJpg():
    def __init__(self, device, w=1920, h=1080):
        # init v4l2mjpg library
        libpath = os.path.join(ROOT, 'lib', 'libv4l2mjpg.so')
        lib = ct.CDLL(libpath, use_errno=True)

        self.__open = lib.v4l2_open
        self.__open.argtypes = [
            ct.c_char_p,
            ct.c_ushort,
            ct.c_ushort,
            ct.POINTER(ct.c_uint),
            ct.POINTER(ct.c_uint)
        ]
        self.__open.restype = ct.c_void_p

        self.__start = lib.v4l2_start
        self.__start.argtypes = [
            ct.c_void_p
        ]
        self.__start.restype = ct.c_int

        self.__dqbuf = lib.v4l2_dqbuf
        self.__dqbuf.argtypes = [
            ct.c_void_p,
            ct.c_uint,
            ct.POINTER(ct.c_size_t),
            ct.POINTER(ct.c_uint),
            ct.POINTER(ct.c_uint)
         ]
        self.__dqbuf.restype = ct.c_void_p

        self.__qbuf = lib.v4l2_qbuf
        self.__qbuf.argtypes = [
            ct.c_void_p
        ]
        self.__qbuf.restype = ct.c_int

        self.__stop = lib.v4l2_stop
        self.__stop.argtypes = [
            ct.c_void_p
        ]
        self.__stop.restype = ct.c_int

        self.__close = lib.v4l2_close
        self.__close.argtypes = [
            ct.c_void_p
        ]
        self.__close.restype = None

        num = ct.c_uint(0)
        den = ct.c_uint(0)

        self.__handle = self.__open(device.encode('utf-8'), w, h, num, den)
        if self.__handle is None:
            errno = ct.get_errno()
            error = ('Failed to open device {}: [Errno {}] {}'
                     .format(device, errno, os.strerror(errno)))
            raise Exception(error)

        self.fps = (num.value, den.value)

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def start(self):
        if self.__start(self.__handle) == -1:
            errno = ct.get_errno()
            error = ('Failed to start stream: [Errno {}] {}'
                     .format(errno, os.strerror(errno)))
            raise Exception(error)

    def dqbuf(self):
        w = ct.c_uint(0)
        h = ct.c_uint(0)
        size = ct.c_size_t(0)
        addr = self.__dqbuf(self.__handle, 5000, size, w, h)
        if addr is None:
            errno = ct.get_errno()
            error = ('Failed DQBUF: [Errno {}] {}'
                     .format(errno, os.strerror(errno)))
            raise Exception(error)

        return addr, size.value, w.value, h.value

    def qbuf(self):
        if self.__qbuf(self.__handle) == -1:
            errno = ct.get_errno()
            error = ('Failed QBUF: [Errno {}] {}'
                     .format(errno, os.strerror(errno)))
            raise Exception(error)

    def stop(self):
        if self.__stop(self.__handle) == -1:
            errno = ct.get_errno()
            error = ('Failed to stop stream: [Errno {}] {}'
                     .format(errno, os.strerror(errno)))
            raise Exception(error)

    def close(self):
        if self.__handle is not None:
            self.__close(self.__handle)
            self.__handle = None
