#!/usr/bin/python3
import argparse
import matplotlib.pyplot as plt
from femu_stat import *

ploted = 0
read_bw = []
write_bw = []
afdm_read_bw = []
afdm_write_bw = []
total_read_bw = []

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
                    prog='plot_stat',
                    description='plot statistics from stat json file')
    parser.add_argument('stat_file', help='stat json file')
    parser.add_argument('--range', '-r', help='range of ploted points, use the same syntax as python, e.g. 0:100, 0:, :100. multiple range canbe seperated by ,', default='', type=str)
    args = parser.parse_args()

    try:
        f = open(args.stat_file, 'r')
        json_stat = json.load(f)
    except Exception as e:
        print(e)
        exit(0)

    ranges = []
    # parse range
    if args.range == '':
        ranges.append((0, len(json_stat)))
    else:
        for r in args.range.split(','):
            if ':' in r:
                start, end = r.split(':')
                if start == '':
                    start = 0
                else:
                    start = int(start)
                if end == '':
                    end = len(json_stat)
                else:
                    end = int(end)
                ranges.append((start, end))
            else:
                ranges.append((int(r), int(r)+1))

    for start, end in ranges:
        for stat in json_stat[start:end]:
            read_bw.append(MB(stat['rw']['read_bytes']))
            write_bw.append(MB(stat['rw']['write_bytes']))
            afdm_read_bw.append(MB(stat['afdm_rw']['read_bytes']))
            afdm_write_bw.append(MB(stat['afdm_rw']['write_bytes']))
            total_read_bw.append(MB(stat['rw']['read_bytes']+stat['afdm_rw']['read_bytes']))
            ploted += 1

    print('Ploted %d points' % ploted)
    plt.rcParams['savefig.dpi'] = 300
    # plt.rcParams['savefig.format'] = 'svg'
    fig = plt.figure()
    ax1 = fig.add_subplot(1,1,1)
    ax1.xaxis.set_major_locator(plt.MaxNLocator(integer=True))
    ax1.yaxis.grid(True)
    ax1.set_xlabel('Seconds')
    ax1.set_ylabel('Bandwidth (MB/s)')
    ax1.plot(total_read_bw, color='y', label='total read')
    ax1.plot(read_bw, color='r', label='read')
    ax1.plot(write_bw, color='b', label='write')
    ax1.plot(afdm_read_bw, color='g', label='afdm read')
    ax1.legend(['total read', 'read', 'write', 'afdm read'])
    plt.savefig('stat.png')
