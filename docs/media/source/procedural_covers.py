"""Original, self-contained audio/artwork for the GODENplayer README shoot.
No samples, personal media or external artwork. Run serially with one encoder.
"""
from pathlib import Path
import hashlib
import json
import math
import subprocess
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
MEDIA = ROOT / 'demo-library'
RATE = 48000
DURATION = 64
FFMPEG = Path('ffmpeg')
ALBUMS = [
    ('Soft Current', 'Ambient', (9, 26, 37), (60, 210, 192), 57, ['First Light', 'Suspended Lines', 'Undertow']),
    ('Amber Circuit', 'Electronic', (30, 14, 21), (246, 155, 69), 60, ['Copper Hour', 'Afterimage', 'Warm Signal']),
    ('Night Geometry', 'Electronic', (10, 10, 34), (144, 114, 244), 62, ['Nightfall', 'Glass Arc', 'Quiet Motion']),
    ('Contour Studies', 'Ambient', (31, 30, 27), (220, 210, 181), 55, ['Drift Pattern', 'Paper Horizon', 'Slow Orbit']),
    ('Phase Garden', 'Downtempo', (11, 27, 25), (171, 213, 96), 64, ['Signal Bloom', 'Open Form', 'Field Notes']),
    ('Prism Notes', 'Downtempo', (29, 12, 31), (235, 102, 155), 59, ['Rose Window', 'Chromatic Air', 'Returning']),
]

def font(size, bold=False):
    return ImageFont.truetype('C:/Windows/Fonts/seguisb.ttf' if bold else 'C:/Windows/Fonts/segoeui.ttf', size)

def artwork(index, title, dark, accent, target):
    n = 1600
    y, x = np.mgrid[0:n, 0:n].astype(np.float32) / n
    angle = 0.32 + index * 0.25
    a = (x - .55) * math.cos(angle) + (y - .44) * math.sin(angle)
    radius = np.sqrt(((x-.53)*1.03)**2 + ((y-.42)*1.05)**2)
    glow = np.exp(-((a+.13)**2/.12 + (radius-.28)**2/.08))
    field = .17 + .64*glow + .09*np.sin(a*12+radius*8)
    c = np.array(dark, np.float32)[None,None,:] + field[:,:,None] * (np.array(accent,np.float32)-np.array(dark,np.float32))[None,None,:]
    rng = np.random.default_rng(2210 + index)
    c += rng.normal(0, .6, (n,n,1)).astype(np.float32)
    image = Image.fromarray(np.uint8(np.clip(c,0,255)))
    draw = ImageDraw.Draw(image, 'RGBA')
    # Deliberately broad geometric forms survive both thumbnails and GlyphFlow.
    center = (810 + 90*math.sin(index), 650 + 55*math.cos(index))
    for k in range(6):
        r = 215 + k*53
        draw.arc((center[0]-r,center[1]-r,center[0]+r,center[1]+r),
            190+index*13, 500-index*9, fill=(*accent, 145-k*12), width=8 if k%2 else 15)
    draw.ellipse((center[0]-144,center[1]-144,center[0]+144,center[1]+144), fill=(*dark,210), outline=(*accent,180), width=3)
    for k in range(9):
        yy=1010+k*29
        draw.line([(96,yy),(370,yy-70),(740,yy-25),(1260,yy-135),(1500,yy-100)],fill=(*accent,35+k*3),width=2)
    draw.text((104,92),'G O D E N   /   D E M O   S E S S I O N S',font=font(29),fill=(240,240,234,225))
    words=title.split(); lines=['']
    for word in words:
        test=(lines[-1]+' '+word).strip()
        if draw.textlength(test,font=font(116,True))>1370: lines.append(word)
        else: lines[-1]=test
    yy=1292-(len(lines)-1)*126
    for line in lines:
        draw.text((96,yy),line,font=font(116,True),fill=(249,246,237,255)); yy+=126
    draw.text((102,1492),f'ORIGINAL PROCEDURAL SERIES  /  {index+1:02d}',font=font(25),fill=(238,238,229,200))
    image.save(target, optimize=True)

