#!/usr/bin/env python3

# The MIT License (MIT)
# =====================
#
# Copyright © 2019 Azavea
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation
# files (the “Software”), to deal in the Software without
# restriction, including without limitation the rights to use,
# copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following
# conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
# OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.


import argparse
import json
import os


def cli_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument('--metadata-file', required=True, type=str)
    parser.add_argument('--input', required=True, type=str)
    parser.add_argument('--output', required=True, type=str)
    return parser


# Input given an input file and a metadata-donor file, produce an
# output file with the contents of the input file clipped the extent
# of the donor file.
if __name__ == '__main__':
    args = cli_parser().parse_args()
    command = 'gdalinfo -proj4 -json {}'.format(args.metadata_file)
    gdalinfo = json.loads(os.popen(command).read())
    proj4 = gdalinfo['coordinateSystem']['proj4']
    [width, height] = gdalinfo['size']
    [xmin, ymax] = gdalinfo['cornerCoordinates']['upperLeft']
    [xmax, ymin] = gdalinfo['cornerCoordinates']['lowerRight']
    os.system('gdalwarp {} -dstnodata 255 -t_srs "{}" -ts {} {} -te {} {} {} {} -r near -co COMPRESS=LZW -co PREDICTOR=2 -co SPARSE_OK=YES {}'.format(
        args.input, proj4, width, height, xmin, ymin, xmax, ymax, args.output))
