#!/usr/bin/python3
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import socket
import pickle
from threading import Thread
from femu_stat import *

server_port = 5424
server_url = '192.168.1.22'

per_sec_stat = []
num_stat = 0
kill_client = False

def receive_stat_data():
    global per_sec_stat, num_stat, kill_client
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((server_url, server_port))
        s.settimeout(0.2)
        print('connected')
        while True:
            try:
                if kill_client:
                    s.sendall(b'exit')
                    break
                s.sendall(b'heartbeat')
                sz = s.recv(4)
                sz = int.from_bytes(sz, 'little')
                data = b''
                while len(data) < sz:
                    data += s.recv(4096)
                if len(data) == 4 and data == b'exit':
                    print('server disconnected')
                    break
                sec_stat = pickle.loads(data)
                print(sec_stat)
                per_sec_stat.append(sec_stat)
                num_stat += 1
            except socket.timeout:
                continue
            except socket.error as e:
                print(e)
                break
            except Exception as e:
                print(e)
                continue

sock_thread = Thread(target=receive_stat_data)
sock_thread.start()

ploted = 0
read_bw = []
write_bw = []
afdm_read_bw = []
afdm_write_bw = []
total_read_bw = []

plt.rcParams['savefig.dpi'] = 300
# plt.rcParams['savefig.format'] = 'svg'
fig = plt.figure()
ax1 = fig.add_subplot(1,1,1)
ax1.xaxis.set_major_locator(plt.MaxNLocator(integer=True))
ax1.yaxis.grid(True)
ax1.set_xlabel('Seconds')
ax1.set_ylabel('Bandwidth (MB/s)')

def update_data(i):
    global ploted, num_stat, per_sec_stat, x, y, ax1
    if ploted < num_stat:
        stat = per_sec_stat[ploted]
        read_bw.append(MB(stat.rw.read_bytes))
        write_bw.append(MB(stat.rw.write_bytes))
        afdm_read_bw.append(MB(stat.afdm_rw.read_bytes))
        afdm_write_bw.append(MB(stat.afdm_rw.write_bytes))
        total_read_bw.append(MB(stat.rw.read_bytes+stat.afdm_rw.read_bytes))
        ploted += 1
        # ax1.plot(total_read_bw, label='total read')
        # ax1.plot(total_write_bw, label='total read')
        ax1.plot(read_bw, color='y', label='read')
        ax1.plot(write_bw, color='r', label='write')
        ax1.plot(afdm_read_bw, color='g', label='afdm read')
        ax1.plot(afdm_write_bw, color='b', label='afdm write')
        # ax1.legend(['total read', 'read', 'write', 'afdm read'])
        ax1.legend(['read', 'write', 'afdm read', 'afdm write'])

ani = animation.FuncAnimation(fig, update_data, interval=100, save_count=1024)
plt.show()

kill_client = True
sock_thread.join()