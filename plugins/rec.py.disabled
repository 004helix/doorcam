from datetime import datetime
import collections
import subprocess
import threading
import queue
import time
import yaml
import os
import os.path


#
# Config file location
#
ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..'
))
if os.getenv('REC_CFG') is not None:
    CFG = os.getenv('REC_CFG')
else:
    CFG = os.path.join(ROOT, 'etc', 'rec.yml')


#
# Font file
#
FONT = os.path.join(ROOT, 'share', 'RobotoMono-Regular.ttf')


#
# Parse config file
#
with open(CFG, 'r') as f:
    DIR = yaml.safe_load(f)['dir']

if not os.path.isdir(DIR):
    raise Exception(f'Directory {DIR} not found or not a directory')


#
# Recorder
#
class Recorder:
    def __init__(self, logger, fps):
        self.q = queue.Queue()
        self.fps = fps
        self.log = logger
        threading.Thread(target=self.worker, daemon=True).start()

    def put(self, frame):
        self.q.put((frame, False))

    def cache(self, frame):
        self.q.put((frame, True))

    def stop(self):
        self.q.put((None, None))

    def sizeof_fmt(self, num, suffix='B'):
        for unit in ('', 'Ki', 'Mi', 'Gi', 'Ti', 'Pi', 'Ei', 'Zi'):
            if abs(num) < 1024.0:
                return f'{num:3.1f}{unit}{suffix}'
            num /= 1024.0
        return f'{num:.1f}Yi{suffix}'

    def worker(self):
        name = datetime.now().strftime('%F_%H.%M.%S')

        self.log.info('recording started')

        # start encoder
        vfilter = (
            'scale_vaapi=format=nv12,hwmap=mode=read+write+direct,'
            f'drawtext=fontfile={FONT}:'
            'x=20:y=20:fontcolor=white:fontsize=32:'
            'shadowcolor=black:shadowx=-2:shadowy=-2:'
            "text='%{metadata\\:DateTimeOriginal}."
            "%{metadata\\:SubSecTimeOriginal}',"
            'format=nv12,hwmap'
        )
        cmd = [
            'ffmpeg', '-nostdin', '-nostats', '-hide_banner',
            '-loglevel', 'warning',
            '-hwaccel', 'vaapi',
            '-hwaccel_device', '/dev/dri/renderD128',
            '-hwaccel_output_format', 'vaapi',
            '-r', f'{self.fps[0]}/{self.fps[1]}',
            '-f', 'mpjpeg', '-i', '-', '-vf', vfilter,
            '-c:v', 'vp9_vaapi', '-b:v', '5M',
            '-f', 'ivf', '-'
        ]
        # use libva-intel-driver for VP9
        env = os.environ.copy()
        #env['LIBVA_DRIVER_NAME'] = 'i965'
        encoder = subprocess.Popen(cmd,
                                   env=env,
                                   stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE)

        self.log.info('    -> {}'.format(' '.join(cmd)))

        # start writer
        tmp = os.path.join(DIR, f'.{name}.webm')
        cmd = [
            'ffmpeg', '-nostdin', '-nostats', '-hide_banner',
            '-loglevel', 'warning',
            '-r', '{}/{}'.format(self.fps[0], self.fps[1]),
            '-f', 'ivf', '-i', '-',
            '-c', 'copy', tmp
        ]
        writer = subprocess.Popen(cmd,
                                  stdin=subprocess.PIPE)

        self.log.info('    -> {}'.format(' '.join(cmd)))

        # cache
        c = collections.deque()
        csize = 0

        # perf counters
        maxqsize = 0
        maxcsize = 0

        # IVF header (32 bytes)
        ivf_header = None

        # recorder loop
        while True:
            qsize = self.q.qsize()

            if qsize > maxqsize:
                maxqsize = qsize

            frame, cache = self.q.get()

            if frame is None:
                self.q.task_done()
                break

            # write jpeg frame to encoder
            os.write(encoder.stdin.fileno(), frame)
            del frame

            # read ivf(vp9) frame from encoder
            if ivf_header is None:
                ivf_header = encoder.stdout.read(32)
                os.write(writer.stdin.fileno(), ivf_header)

            ivf_frame_header = encoder.stdout.read(12)
            vp9_frame_size = int.from_bytes(ivf_frame_header[:4],
                                            byteorder='little')

            ivf_frame = ivf_frame_header + encoder.stdout.read(vp9_frame_size)

            # cache ivf frame
            if cache:
                c.append(ivf_frame)
                csize += len(ivf_frame)
                if maxcsize < csize:
                    maxcsize = csize
                self.q.task_done()
                del ivf_frame
                continue

            # flush ivf frame cache
            while len(c) > 0:
                cached_frame = c.popleft()
                os.write(writer.stdin.fileno(), cached_frame)
                csize -= len(cached_frame)
                del cached_frame

            # write ivf frame to writer
            os.write(writer.stdin.fileno(), ivf_frame)
            self.q.task_done()
            del ivf_frame

        # recording finished

        # clear cache
        c.clear()

        # close encoder and writer
        encoder.stdin.close()
        encoder.stdout.read()
        encoder.wait()
        writer.stdin.close()
        writer.wait()

        self.log.info((
            'recording finished ('
            f'max queue size: {maxqsize}, '
            'max cache size: ' + self.sizeof_fmt(maxcsize) + ')'
        ))

        dst = os.path.join(DIR, f'{name}.webm')
        self.log.info(f'rename {tmp}')
        self.log.info(f'    -> {dst}')
        os.rename(tmp, dst)


#
# Rec plugin
#
class Plugin:
    def __init__(self, logger, release_cb, initial_width, initial_height, fps):
        # doorcam plugin interface
        self.cb = release_cb
        self.log = logger
        self.fps = fps

        # frame queue (cache)
        self.q = collections.deque()

        # the number of pre-captured (buffered) frames from before motion
        self.ql = 3 * fps[0] // fps[1]

        # gap in frames of no motion detected that triggers the end of an event
        self.gap = 3 * fps[0] // fps[1]

        # number of frames between motion events to write in one file
        # 30 sec
        self.glue = 30 * fps[0] // fps[1]

        # recorder
        self.rec = None
        self.rec_gap = 0
        self.rec_glue = 0

        # mpjpeg header
        self.mpjpeg_header = bytearray(
            b'--doorcam-rec\r\n'
            b'Content-Type: image/jpeg\r\n'
            b'Content-Length: '
        )

        # jpeg exif
        self.exif = bytearray.fromhex(
            'FF D8'                                # SOI marker
            'FF E1'                                # APP1 marker
            '00 54'                                # APP1 size
            '45 78 69 66 00 00'                    # Exif header
            '4D 4D 00 2A 00 00 00 08'              # TIFF header
            '00 01'                                # IFD0 (1 element)
            '87 69 00 04 00 00 00 01 00 00 00 1A'  # ExifOffset
            '00 00 00 00'                          # End of Link
            '00 02'                                # Exif SubIFD (2 elements)
            '90 03 00 02 00 00 00 14 00 00 00 38'  # DateTimeOriginal
            '92 91 00 02 00 00 00 04 00 00 00 00'  # SubSecTimeOriginal
            '00 00 00 00'
            '00 00 00 00 00 00 00 00 00 00'        # DateTimeOriginal value
            '00 00 00 00 00 00 00 00 00 00'
        )

        self.log.info('recorder started')
        self.log.info(f'    -> {DIR}')


    def release(self):
        self.cb()

    def process(self, ts, jpeg, w, h, motion):
        ''' assume jpeg has no APP1/exif '''

        # save metadata (DateTimeOriginal + SubSecTimeOriginal)
        t = datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S.%f')
        b = bytearray(t.encode())
        self.exif[68:87] = b[0:19]
        self.exif[60:63] = b[20:23]

        # calculate jpeg image size
        content_length = len(self.exif) + len(jpeg) - 2

        # copy frame and release original image
        frame = self.mpjpeg_header + \
            f'{content_length}\r\n\r\n'.encode() + \
            self.exif + jpeg[2:]
        self.release()

        # append frame to queue
        if len(self.q) >= self.ql:
            self.q.popleft()

        self.q.append(frame)

        if self.rec is not None:
            if self.rec.q.qsize() > self.gap * 4:
                self.log.error(f'recorder qsize too big: {self.rec.q.qsize()}')
                self.log.error(('your CPU/GPU is too slow or busy, '
                                'pls check encoder options'))
                self.rec.put((None, None))
                self.rec = None

        # check motion was detected
        if motion:
            if self.rec is None:
                # start recorder
                self.rec = Recorder(self.log, self.fps)
                for f in self.q:
                    self.rec.put(f)
            else:
                # send frame to recorder
                self.rec.put(frame)

            # update gap and glue counters
            self.rec_gap = self.gap
            self.rec_glue = self.glue
            return

        # no motion and no recorder
        if self.rec is None:
            return

        # check motion event end
        if self.rec_gap > 0:
            self.rec_gap -= 1
            self.rec.put(frame)
            return

        # decrease glue counter
        self.rec_glue -= 1

        # check glue counter
        if self.rec_glue == 0:
            # stop recording
            self.rec.stop()
            self.rec = None
        else:
            # cache frame in recorder
            self.rec.cache(frame)
