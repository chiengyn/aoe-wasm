#!/usr/bin/env python3
from pathlib import Path
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
import argparse
import base64
import hashlib
import struct
import threading
from dplay_relay import Relay, ProtocolError, MAX_FRAME
relay=Relay()
root=Path(__file__).resolve().parents[1]/'web'
p=argparse.ArgumentParser()
p.add_argument('--port',type=int,default=8788)
p.add_argument('--host',default='127.0.0.1',help='Address to listen on; 0.0.0.0 makes the game reachable on the local network')
a=p.parse_args()
WS_GUID=b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'  # RFC 6455

class WebSocket:
    """Minimal RFC 6455 server side: binary messages, ping/pong, close. One per game connection."""
    def __init__(self,handler):
        self.handler,self.lock,self.session=handler,threading.Lock(),None
    def send(self,data,opcode=2):
        header=bytes([0x80|opcode])
        n=len(data)
        header+=bytes([n]) if n<126 else bytes([126])+struct.pack('>H',n) if n<65536 else bytes([127])+struct.pack('>Q',n)
        with self.lock:
            try:self.handler.wfile.write(header+data);self.handler.wfile.flush()
            except OSError:pass
    def frames(self):
        rfile,message,kind=self.handler.rfile,b'',None
        while True:
            head=rfile.read(2)
            if len(head)<2:return
            fin,opcode,masked,n=head[0]&0x80,head[0]&0x0f,head[1]&0x80,head[1]&0x7f
            if n==126:n=struct.unpack('>H',rfile.read(2))[0]
            elif n==127:n=struct.unpack('>Q',rfile.read(8))[0]
            if not masked or n>MAX_FRAME:return
            mask=rfile.read(4);payload=bytearray(rfile.read(n))
            if len(payload)<n:return
            for i in range(n):payload[i]^=mask[i&3]
            if opcode==8:self.send(b'',8);return
            if opcode==9:self.send(bytes(payload),10);continue
            if opcode==10:continue
            if opcode in(1,2):message,kind=bytes(payload),opcode
            elif opcode==0:message+=bytes(payload)
            if len(message)>MAX_FRAME:return
            if fin and kind==2:yield message
            if fin:message,kind=b'',None

class Handler(SimpleHTTPRequestHandler):
    def __init__(self,*args,**kw):super().__init__(*args,directory=str(root),**kw)
    def end_headers(self):
        self.send_header('Cache-Control','no-cache')
        self.send_header('X-Content-Type-Options','nosniff')
        super().end_headers()
    def same_origin(self):
        # https: when the page is served through a TLS reverse proxy that passes the Host header on.
        host=self.headers.get('Host','')
        return self.headers.get('Origin') in (None,'http://'+host,'https://'+host)
    def do_GET(self):
        if self.path.split('?')[0]!='/api/dplay':return super().do_GET()
        # DirectPlay relay for the real game (web/native-dplay.mjs <-> tools/dplay_relay.py).
        key=self.headers.get('Sec-WebSocket-Key')
        if self.headers.get('Upgrade','').lower()!='websocket' or not key or not self.same_origin():
            self.send_error(400);return
        accept=base64.b64encode(hashlib.sha1(key.encode()+WS_GUID).digest()).decode()
        # Upgrades need HTTP/1.1; the base handler speaks HTTP/1.0.
        self.wfile.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                          'Sec-WebSocket-Accept: '+accept+'\r\n\r\n').encode())
        self.wfile.flush()
        conn=WebSocket(self)
        try:
            for frame in conn.frames():relay.handle(conn,frame)
        except (ProtocolError,OSError,struct.error):pass
        finally:relay.disconnect(conn);self.close_connection=True
    def log_message(self,fmt,*args):
        if not self.path.startswith('/api/dplay'):super().log_message(fmt,*args)
server=ThreadingHTTPServer((a.host,a.port),Handler)
server.daemon_threads=True
print(f'Open http://{"127.0.0.1" if a.host in ("0.0.0.0","") else a.host}:{a.port}',flush=True)
try:server.serve_forever()
except KeyboardInterrupt:pass
