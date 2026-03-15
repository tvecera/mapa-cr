#!/usr/bin/env python3
"""Local dev server for testing config.html with TMEP API proxy."""

import json
import os
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import parse_qs
from urllib.request import urlopen, Request

PORT = 8090
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')

PROXY_ROUTES = {
    '/api/okresy': 'http://cdn.tmep.cz/app/export/okresy-cr-vse.json',
    '/api/srazky': 'http://tmep.cz/app/export/okresy-srazky-laskakit.json',
}

START_TIME = time.time()

MOCK_CONFIG = {
    'ssid': 'DevWiFi',
    'param': 'h1',
    'brightness': 50,
    'update': 60,
    'nightStart': 22,
    'nightEnd': 6,
    'nightBright': 0,
}

MOCK_STATS = {
    'h1': {'valid': True, 'min': -2.5, 'avg': 8.3, 'max': 15.1},
    'h2': {'valid': True, 'min': 45.0, 'avg': 68.0, 'max': 92.0},
    'h3': {'valid': True, 'min': 1005.0, 'avg': 1018.0, 'max': 1028.0},
    'h4': {'valid': True, 'min': 5.0, 'avg': 22.0, 'max': 45.0},
    'meteoradar': {'valid': False},
}


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def do_GET(self):
        if self.path in PROXY_ROUTES:
            self._proxy(PROXY_ROUTES[self.path])
        elif self.path == '/api/stats':
            self._json(MOCK_STATS.get(MOCK_CONFIG['param'], {'valid': False}))
        elif self.path == '/api/config':
            self._json(MOCK_CONFIG)
        elif self.path == '/api/wifi-scan':
            self._json([
                {'ssid': 'HomeWiFi', 'rssi': -45, 'ch': 6, 'enc': 3},
                {'ssid': 'Neighbors5G', 'rssi': -62, 'ch': 36, 'enc': 3},
                {'ssid': 'CafeHotspot', 'rssi': -71, 'ch': 1, 'enc': 0},
                {'ssid': 'Office_Guest', 'rssi': -78, 'ch': 11, 'enc': 4},
                {'ssid': '', 'rssi': -80, 'ch': 3, 'enc': 3},
            ])
        elif self.path == '/api/info':
            self._json({
                'ip': '127.0.0.1',
                'mac': 'DE:AD:BE:EF:00:00',
                'rssi': 0,
                'heap': 0,
                'uptime': int(time.time() - START_TIME),
                'version': '1.0.0',
            })
        else:
            super().do_GET()

    def do_POST(self):
        if self.path == '/api/ota-update':
            length = int(self.headers.get('Content-Length', 0))
            if length:
                self.rfile.read(length)
            self._json({'ok': True})
        elif self.path in ('/api/config', '/save'):
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode() if length else ''
            params = parse_qs(body)
            old_ssid = MOCK_CONFIG['ssid']
            has_new_password = 'password' in params and params['password'][0]
            for key in MOCK_CONFIG:
                if key in params:
                    val = params[key][0]
                    if key in ('brightness', 'update', 'nightStart', 'nightEnd', 'nightBright'):
                        MOCK_CONFIG[key] = int(val)
                    else:
                        MOCK_CONFIG[key] = val
            restart = (MOCK_CONFIG['ssid'] != old_ssid) or bool(has_new_password)
            self._json({'restart': restart})
        else:
            self.send_response(404)
            self.end_headers()

    def _proxy(self, url):
        try:
            req = Request(url, headers={'User-Agent': 'LaskaKit-MapaCR-Dev/1.0'})
            with urlopen(req, timeout=10) as resp:
                data = resp.read()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as e:
            msg = str(e).encode()
            self.send_response(502)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(msg)))
            self.end_headers()
            self.wfile.write(msg)

    def _json(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == '__main__':
    os.chdir(WEB_DIR)
    server = HTTPServer(('', PORT), Handler)
    print(f'Dev server: http://localhost:{PORT}/')
    print(f'Serving files from: {WEB_DIR}')
    print(f'Proxy: /api/okresy  -> {PROXY_ROUTES["/api/okresy"]}')
    print(f'Proxy: /api/srazky  -> {PROXY_ROUTES["/api/srazky"]}')
    print(f'Mock:  /api/stats, /api/config, /api/info, /api/wifi-scan, /api/ota-update, /save')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nStopped.')
