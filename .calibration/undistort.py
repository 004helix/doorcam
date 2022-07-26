#!/usr/bin/python3

import os
import sys

root = os.path.dirname(os.path.abspath(__file__))
lib = os.path.abspath(os.path.join(root, '../lib'))
sys.path.insert(0, lib)

import numpy as np
import base64
import cv2
import glob
import io

# termination criteria
criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.01)

rows = 0
cols = 0
imgs = list()

def calibrate(dirpath, width=9, height=6):
    global rows, cols, imgs

    """ Apply camera calibration operation for images in the given directory path. """
    # prepare object points, like (0,0,0), (1,0,0), (2,0,0) ....,(8,6,0)
    objp = np.zeros((height * width, 3), np.float32)
    objp[:,:2] = np.mgrid[0:width, 0:height].T.reshape(-1, 2)

    # Arrays to store object points and image points from all the images.
    objpoints = []  # 3d point in real world space
    imgpoints = []  # 2d points in image plane.

    images = glob.glob(os.path.join(dirpath, '*.jpg'))

    for fname in sorted(images):
        img = cv2.imread(fname, cv2.IMREAD_GRAYSCALE)

        # Find the chess board corners
        ret, corners = cv2.findChessboardCorners(img, (width, height), None)

        # If found, add object points, image points (after refining them)
        if ret:
            print(fname, "chessboard found")
            objpoints.append(objp)

            corners2 = cv2.cornerSubPix(img, corners, (11,11), (-1,-1), criteria)
            imgpoints.append(corners2)

            imgs.append(fname)
            rows, cols = img.shape[:2]
        else:
            print(fname, "chessboard not found")

    ret, mtx, dist, rvecs, tvecs = cv2.calibrateCamera(objpoints, imgpoints, img.shape[::-1], None, None)

    return [ret, mtx, dist, rvecs, tvecs]


if __name__ == '__main__':
    image_dir = os.path.join(root, 'chessboards')
    ret, mtx, dist, rvecs, tvecs = calibrate(image_dir, 9, 6)
    print("Calibration is finished\n")
    print("RMS:", ret)

    newcammtx, roi = cv2.getOptimalNewCameraMatrix(mtx, dist, (cols, rows), 0, (cols, rows))
    _, _, w, h = roi
    print("ROI: 1920x1080 -> {}x{}".format(w, h))

    mapx, mapy = cv2.initUndistortRectifyMap(mtx, dist, None, newcammtx, (cols, rows), 5)

    for fname in imgs:
        img = cv2.imread(fname)
        dst = cv2.remap(img, mapx, mapy, cv2.INTER_LINEAR)
        dstpath = os.path.join(root, 'result', os.path.basename(fname))
        cv2.imwrite(dstpath, dst)

    #print("\nCompressed KD.npz:")
    #npz = io.BytesIO()
    #np.savez_compressed(npz, K=mtx, D=dist)
    #npz.seek(0)
    #base64.encode(npz, sys.stdout.buffer)

    cv_file = cv2.FileStorage('/dev/shm/kd.yml', cv2.FILE_STORAGE_WRITE)
    cv_file.write("K", mtx)
    cv_file.write("D", dist)
    cv_file.release()

    with open('/dev/shm/kd.yml', 'r') as f:
        print('\nUndistortion matrix:\n')
        sys.stdout.write(f.read())

    print(cols, rows)

    os.unlink('/dev/shm/kd.yml')
