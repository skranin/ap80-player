import usb.core, usb.util, struct, sys, time
import usb.backend.libusb1 as lb
be = lb.get_backend(find_library=lambda x: "/opt/homebrew/lib/libusb-1.0.dylib")
dev = usb.core.find(idVendor=0x18d1, backend=be) or usb.core.find(idVendor=0xc502, backend=be)
if not dev: sys.exit("no device")
print("dev %04x:%04x" % (dev.idVendor, dev.idProduct))
cfg = dev.get_active_configuration()
intf = None
for i in cfg:
    print(" intf", i.bInterfaceNumber, "class %02x/%02x/%02x" % (i.bInterfaceClass, i.bInterfaceSubClass, i.bInterfaceProtocol), [hex(e.bEndpointAddress) for e in i])
    if (i.bInterfaceClass, i.bInterfaceSubClass, i.bInterfaceProtocol) == (0xff, 0x42, 0x01): intf = i
if intf is None: sys.exit("no adb interface")
try: usb.util.claim_interface(dev, intf.bInterfaceNumber)
except Exception as e: print("claim:", e)
ep_out = [e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT][0]
ep_in = [e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN][0]
def msg(cmd, a0, a1, data=b""):
    c = struct.unpack("<I", cmd)[0]
    return struct.pack("<6I", c, a0, a1, len(data), sum(data) & 0xffffffff, c ^ 0xffffffff), data
def send(cmd, a0, a1, data=b""):
    h, d = msg(cmd, a0, a1, data); ep_out.write(h, 2000)
    if d: ep_out.write(d, 2000)
def recv(t=3000):
    h = bytes(ep_in.read(24, t)); cmd, a0, a1, n, _, _ = struct.unpack("<6I", h)
    d = bytes(ep_in.read(n, t)) if n else b""
    return struct.pack("<I", cmd), a0, a1, d
send(b"CNXN", 0x01000000, 4096, b"host::\0")
for _ in range(3):
    try: print("recv:", recv())
    except Exception as e: print("recv err:", e); break
if len(sys.argv) > 1:
    send(b"OPEN", 1, 0, ("shell:" + sys.argv[1] + "\0").encode())
    out = b""
    while True:
        try: c, a0, a1, d = recv(5000)
        except Exception as e: print("  end:", e); break
        print("  <-", c, a0, a1, d[:80])
        if c == b"WRTE": out += d; send(b"OKAY", 1, a0)
        elif c == b"CLSE": break
    print(out.decode(errors="replace"))
