from datetime import datetime
import subprocess
import os
import os.path


ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..'
))
SHARE = os.path.join(ROOT, 'share')
FONT = os.path.join(SHARE, 'RobotoMono-Regular.ttf')



#
# Scale plugin
#
class Plugin:
    def __init__(self, logger, release_cb, initial_width, initial_height, fps):
        self.cb = release_cb
        self.log = logger

        self.mpjpeg_header = bytearray(
            b'--doorcam-scale\r\n'
            b'Content-Type: image/jpeg\r\n'
            b'Content-Length: '
        )

        self.exif = bytearray.fromhex(
            'FF D8'                                # SOI marker
            'FF E1'                                # APP1 marker
            '00 48'                                # APP1 size
            '45 78 69 66 00 00'                    # Exif header
            '4D 4D 00 2A 00 00 00 08'              # TIFF header
            '00 01'                                # IFD0 (1 element)
            '87 69 00 04 00 00 00 01 00 00 00 1A'  # ExifOffset
            '00 00 00 00'                          # End of Link
            '00 01'                                # Exif SubIFD (1 element)
            '90 03 00 02 00 00 00 14 00 00 00 2C'  # DateTimeOriginal
            '00 00 00 00'
            '00 00 00 00 00 00 00 00 00 00'        # DateTimeOriginal value
            '00 00 00 00 00 00 00 00 00 00'
        )

        cmd = ['ffmpjpeg-httpd', '-a', '127.0.0.1', '-p', '8081']
        self.stream1 = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        logger.info('httpd @ 127.0.0.1:8098 started')
        logger.info('    -> {}'.format(' '.join(cmd)))
        mpjpeg_fd = self.stream1.stdin.fileno()

        cmd = ['vp9-streamer', '-A', '127.0.0.1', '-P', '8099', 'br0', 'br10', 'br20']
        self.stream2 = subprocess.Popen(cmd, cwd=SHARE, stdin=subprocess.PIPE)
        logger.info('httpd @ 127.0.0.1:8099 started')
        logger.info('    -> {}'.format(' '.join(cmd)))
        ivf_fd = self.stream2.stdin.fileno()

        vfilter = (
            'scale_vaapi=format=nv12:w=960:h=540,hwmap=mode=read+write+direct,'
            'drawtext=fontfile=' + FONT + ':'
            'x=10:y=10:fontcolor=white:fontsize=20:'
            'shadowcolor=black:shadowx=-2:shadowy=-2:'
            "text='%{metadata\\:DateTimeOriginal}',"
            'format=nv12,hwmap,split[mjpeg][vp9]'
        )
        cmd = [
            'ffmpeg', '-nostdin', '-nostats', '-hide_banner',
            '-loglevel', 'warning',
            '-hwaccel', 'vaapi',
            '-hwaccel_device', '/dev/dri/renderD128',
            '-hwaccel_output_format', 'vaapi',
            '-f', 'mpjpeg', '-i', '-','-filter_complex', vfilter,
            '-map', '[mjpeg]', '-c:v', 'mjpeg_vaapi', '-global_quality', '85', '-f', 'mpjpeg', 'pipe:' + str(mpjpeg_fd),
            '-map', '[vp9]', '-c:v', 'vp9_vaapi', '-b:v', '1M', '-g', '15', '-f', 'ivf', 'pipe:' + str(ivf_fd),
        ]
        # use libva-intel-driver for VP9
        env = os.environ.copy()
        env['LIBVA_DRIVER_NAME'] = 'i965'
        self.scaler = subprocess.Popen(cmd,
                                       env=env,
                                       pass_fds=(mpjpeg_fd, ivf_fd),
                                       stdin=subprocess.PIPE)

        logger.info('scaler started')
        logger.info('    -> {}'.format(' '.join(cmd)))

        self.counter = 0

    def release(self):
        self.cb()

    def process(self, ts, jpeg, width, height, motion):
        if self.counter > 0:
            self.release()
            self.counter -= 1
            return

        self.counter = 4

        # assume jpeg has no exif header
        t = datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')
        content_length = len(self.exif) + len(jpeg) - 2
        self.exif[56:75] = bytearray(t.encode())

        frame = self.mpjpeg_header + \
            f'{content_length}\r\n\r\n'.encode() + \
            self.exif + jpeg[2:]
        self.release()

        os.write(self.scaler.stdin.fileno(), frame)
