#!/usr/bin/env python3

import argparse
import os
import sys
import threading
import subprocess
import time
import multiprocessing
from pprint import pprint

if 'DAALA_ROOT' not in os.environ:
    print("Please specify the DAALA_ROOT environment variable to use this tool.")
    sys.exit(1)

daala_root = os.environ['DAALA_ROOT']

class Slot:
    def __init__(self):
        self.name='localhost'
        self.p = None
    def execute(self, work):
        self.work = work
        output_name = work.filename+'.'+str(work.quality)+'.ogv'
        env = {}
        env['DAALA_ROOT'] = daala_root
        env['x'] = str(work.quality)
        print('Encoding',work.filename,'with quality',work.quality)
        self.p = subprocess.Popen([daala_root+'/tools/metrics_gather.sh',work.filename], env=env, stdout=subprocess.PIPE)
    def busy(self):
        if self.p is None:
            return False
        elif self.p.poll() is None:
            return True
        else:
            return False
    def gather(self):
        (stdout, stderr) = self.p.communicate()
        self.work.raw = stdout

class Work:
    def parse(self):
        split = self.raw.decode('utf-8').replace(')',' ').split()
        self.pixels = split[1]
        self.size = split[2]
        self.metric = {}
        self.metric['psnr'] = {}
        self.metric["psnr"][0] = split[6]
        self.metric["psnr"][1] = split[8]
        self.metric["psnr"][2] = split[10]
        self.metric['psnrhvs'] = {}
        self.metric["psnrhvs"][0] = split[14]
        self.metric["psnrhvs"][1] = split[16]
        self.metric["psnrhvs"][2] = split[18]
        self.metric['ssim'] = {}
        self.metric["ssim"][0] = split[22]
        self.metric["ssim"][1] = split[24]
        self.metric["ssim"][2] = split[26]
        self.metric['fastssim'] = {}
        self.metric["fastssim"][0] = split[30]
        self.metric["fastssim"][1] = split[32]
        self.metric["fastssim"][2] = split[34]
        
quality_daala = [1,2,3,4,5,6,7,9,11,13,16,20,25,30,37,45,55,67,81,99,122,148,181,221,270,330]

free_slots = [Slot(),Slot(),Slot(),Slot()]
taken_slots = []

work_items = []
work_done = []


parser = argparse.ArgumentParser(description='Collect RD curve data.')
parser.add_argument('file')
args = parser.parse_args()

for quality in quality_daala:
    work = Work()
    work.quality = quality
    work.filename = args.file
    work_items.append(work)

while(1):
    for slot in taken_slots:
        if slot.busy() == False:
            slot.gather()
            work_done.append(slot.work)
            taken_slots.remove(slot)
            free_slots.append(slot)
    if len(work_items) == 0:
        if len(taken_slots) == 0:
            break
    else:
        if len(free_slots) != 0:
            slot = free_slots.pop()
            work = work_items.pop()
            threading.Thread(slot.execute(work))
            taken_slots.append(slot)
    time.sleep(1)   
    
work_done.sort(key=lambda work: work.quality)
    
f = open(args.file+'.out','w')
for work in work_done:
    work.parse()
    f.write(str(work.quality)+' ')
    f.write(str(work.pixels)+' ')
    f.write(str(work.size)+' ')
    f.write(str(work.metric['psnr'][0])+' ')
    f.write(str(work.metric['psnrhvs'][0])+' ')
    f.write(str(work.metric['ssim'][0])+' ')
    f.write(str(work.metric['fastssim'][0])+' ')
    f.write('\n')
f.close()

