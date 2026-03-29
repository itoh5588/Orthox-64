import socket, time
import sys

def send_qemu(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect("qemu.sock")
        s.recv(1024)
        s.sendall(cmd.encode('utf-8') + b'\n')
        s.close()
    except Exception as e:
        print("error", e)

time.sleep(3)
for char in "hello":
    send_qemu(f"sendkey {char}")
    time.sleep(0.1)
send_qemu("sendkey ret")
