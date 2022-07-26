import ctypes as ct
import threading
import requests
import queue
import time
import os
import os.path
import signal
import yaml
import hmac
import hashlib
import base64


# debug:
#  - motion always detected
DEBUG = False

#
# Get app root directory
#
ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..'
))

#
# Config file location
#
if os.getenv('QRSCAN_CFG') is not None:
    CFG = os.getenv('QRSCAN_CFG')
else:
    CFG = os.path.join(ROOT, 'etc', 'qrscan.yml')

#
# Parse config file
#
SECRETS = None
ACTION = None
DIR = None

with open(CFG, 'r') as f:
    cfg = yaml.safe_load(f)

    SECRETS = cfg['secrets']
    assert isinstance(SECRETS['ttl'], int)
    assert isinstance(SECRETS['bits'], int)
    assert isinstance(SECRETS['keys'], dict)

    ACTION = cfg['action']
    assert isinstance(ACTION['url'], str)
    if 'interval' in ACTION:
        assert isinstance(ACTION['interval'], int)
    if 'method' in ACTION:
        assert isinstance(ACTION['method'], str)
        assert ACTION['method'] in ('GET', 'HEAD', 'PUT', 'POST')
    if 'payload' in ACTION:
        assert isinstance(ACTION['payload'], str)
    if 'headers' in ACTION:
        assert isinstance(ACTION['headers'], dict)

    del cfg

if DIR is not None:
    if not os.path.isdir(DIR):
        raise Exception('Directory {} not found or not a directory'
                        .format(DIR))


#
# QRScan class
#
class QRScan:
    def __init__(self, result_cb):
        path = os.path.join(ROOT, 'lib', 'libDynamsoftBarcodeReader.so')
        libdbr = ct.cdll.LoadLibrary(path)
        path = os.path.join(ROOT, 'lib', 'libqrscan.so')
        libqrscan = ct.cdll.LoadLibrary(path)

        # qrscan_init()
        self.__qrscan_init = libqrscan.qrscan_init
        self.__qrscan_init.argtypes = [
            ct.c_char_p,
            ct.c_char_p, ct.c_char_p,
            ct.c_ushort, ct.c_ushort,
            ct.c_char_p,
        ]
        self.__qrscan_init.restype = ct.c_void_p

        # qrscan_destroy()
        self.__qrscan_destroy = libqrscan.qrscan_destroy
        self.__qrscan_destroy.argtypes = [ct.c_void_p]
        self.__qrscan_destroy.restype = None

        # qrscan_process_jpeg()
        self.__qrscan_process_jpeg = libqrscan.qrscan_process_jpeg
        self.__qrscan_process_jpeg.argtypes = [
            ct.c_void_p, ct.c_void_p, ct.c_size_t
        ]
        self.__qrscan_process_jpeg.restype = ct.c_int

        # qrscan_get_result()
        self.__qrscan_get_result = libqrscan.qrscan_get_result
        self.__qrscan_get_result.argtypes = [ct.c_void_p]
        self.__qrscan_get_result.restype = ct.c_char_p

        # call qrscan_init()
        self.obj = self.__qrscan_init(
            os.path.join(ROOT, 'share', 'undistort.yml').encode('utf-8'),
            os.path.join(ROOT, 'share', 'detect.prototxt').encode('utf-8'),
            os.path.join(ROOT, 'share', 'detect.caffemodel').encode('utf-8'),
            960, 540,  # best detection success rate @ 960x540
            '/dev/dri/renderD128'.encode('utf-8')
        )
        # start worker thread
        self.cb = result_cb
        self.cv = threading.Condition()
        self.frame = None
        self.skipped = 0
        self.processed = 0
        threading.Thread(target=self.worker, daemon=True).start()

    def __del__(self):
        self.__qrscan_destroy(self.obj)

    def process(self, frame):
        with self.cv:
            if self.frame is None:
                self.frame = frame
                self.cv.notify_all()
                self.processed += 1
            else:
                self.skipped += 1

    def worker(self):
        while True:
            with self.cv:
                while self.frame is None:
                    self.cv.wait()

            cnt = self.__qrscan_process_jpeg(
                self.obj,
                self.frame,
                len(self.frame)
            )

            if cnt > 0:
                while True:
                    result = self.__qrscan_get_result(self.obj)

                    if result is None:
                        break

                    self.cb(result)

            with self.cv:
                self.frame = None


#
# QRScan plugin
#
class Plugin:
    def __init__(self, logger, release_cb, initial_width, initial_height, fps):
        self.cb = release_cb
        self.log = logger

        # load qrscan library and start worker thread
        self.qrscan = QRScan(self.qrcb)

        # motion detection values
        self.motion_counter = 0
        self.motion_gap = 5 * fps[0] // fps[1]

        # remove action zombies automatically
        signal.signal(signal.SIGCHLD, signal.SIG_IGN)

        # prepare qr verifier and action runner
        self.qrv = QRVerifier()
        self.action = Action(logger)

        # first frame flag
        self.ff = True

        self.log.info('qrcode scanner started')
        self.log.info(f'    -> {len(self.qrv.keys)} key(s) loaded')

    def release(self):
        self.cb()

    def process(self, ts, jpeg, w, h, motion):
        if DEBUG:
            motion = True

        # process motion detection
        if self.ff:
            self.ff = False
        elif motion:
            self.motion_counter = self.motion_gap
        else:
            if self.motion_counter > 0:
                self.motion_counter -= 1
                if self.motion_counter == 0:
                    self.release()
                    self.log.info(
                        'motion event end (frames processed/skipped '
                        f'{self.qrscan.processed}/{self.qrscan.skipped})'
                    )
                    self.qrscan.processed = 0
                    self.qrscan.skipped = 0
                    return
            else:
                self.release()
                return

        # copy and release jpeg
        frame = jpeg.tobytes()
        self.release()

        self.qrscan.process(frame)

    def qrcb(self, value):
        try:
            text = value.decode('utf-8')
        except Exception:
            text = value

        if self.qrv.verify(value):
            self.log.info('found QR Code "{}", valid'.format(text))
            self.action.run()
        else:
            self.log.info('found QR Code "{}", invalid'.format(text))


#
# QR Code verifier
#
class QRVerifier:
    def __init__(self):
        self.keys = SECRETS['keys']
        self.trim = SECRETS['bits'] // 8
        self.ttl = SECRETS['ttl']

    def verify(self, data):
        if not isinstance(data, bytes):
            return False

        if data.count(b'.') < 2:
            return False

        authid, ts, signature = data.rsplit(b'.', 2)

        if not ts.isdigit():
            return False

        now = int(time.time())

        if int(ts) > now + 10 or int(ts) + self.ttl < now:
            return False

        try:
            auth = authid.decode('utf-8')
        except Exception:
            return False

        if auth not in self.keys.keys():
            return False

        try:
            key = self.keys[auth].encode('utf-8')
        except Exception:
            return False

        h = hmac.new(key, authid + b'.' + ts, hashlib.sha256).digest()
        b = base64.urlsafe_b64encode(h[:self.trim]).rstrip(b'=')

        if b == signature:
            return True

        return False


#
# Do action
#
class Action:
    def __init__(self, logger):
        self.log = logger
        self.url = ACTION['url']
        self.interval = ACTION['interval'] if 'interval' in ACTION else 3
        self.method = ACTION['method'] if 'method' in ACTION else 'GET'
        self.payload = ACTION['payload'] if 'payload' in ACTION else None
        self.headers = ACTION['headers'] if 'headers' in ACTION else {}
        self.last_try = 0.0

    def run(self):
        if self.last_try + self.interval > time.monotonic():
            return

        self.last_try = time.monotonic()

        if os.fork() > 0:
            return

        signal.signal(signal.SIGCHLD, signal.SIG_DFL)

        try:
            kwargs = {'timeout': (10, 10)}
            if len(self.headers) > 0:
                kwargs['headers'] = self.headers
            if self.payload is not None:
                kwargs['data'] = self.payload.encode('utf-8')

            r = requests.request(self.method, self.url, **kwargs)
            r.raise_for_status()

            self.log.info('action finished successfully')

        except Exception as e:
            self.log.error(e, exc_info=True)

        os._exit(0)
