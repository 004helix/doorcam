import subprocess
import time
import os


class Plugin:
    def __init__(self, logger, release_cb, initial_width, initial_height, fps):
        self.cb = release_cb

        cmd = ['ffmpjpeg-httpd', '-a', '127.0.0.1', '-p', '8080']
        self.stream = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        logger.info('httpd @ 127.0.0.1:8080 started')
        logger.info('    -> {}'.format(' '.join(cmd)))

    def release(self):
        self.cb()

    def process(self, ts, jpeg, width, height, motion):
        header = (
            b'--doorcam-raw\r\n'
            b'Content-Type: image/jpeg\r\n'
            b'Content-Length: ' + str(jpeg.nbytes).encode() + b'\r\n\r\n'
        )
        os.writev(self.stream.stdin.fileno(), [header, jpeg])
        self.release()
