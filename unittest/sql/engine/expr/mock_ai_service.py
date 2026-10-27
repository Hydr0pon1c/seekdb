#!/usr/bin/env python3
"""Deterministic HTTP fixture for SQL AI tests; no external model is needed."""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

requests = []
lock = threading.Lock()


def embedding(text, dimension):
    digest = hashlib.sha256(text.encode()).digest()
    return [round((digest[i % len(digest)] - 128) / 128, 6) for i in range(dimension)]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value, status=200):
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        with lock:
            self.reply(list(requests))

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
        if self.path == '/reset':
            with lock:
                requests.clear()
            self.reply({})
            return
        with lock:
            requests.append({'path': self.path, 'body': body})
        if self.path == '/invalid/completion':
            self.reply({'choices': [{'message': {'content': ''}}]})
            return
        if self.path == '/invalid/embedding':
            self.reply({'data': [{'index': 0, 'embedding': [True]}]})
            return
        if self.path == '/invalid/rerank':
            self.reply({'results': [{'index': 1000000, 'relevance_score': 1.0}]})
            return
        if self.path.endswith('/embeddings') or self.path.endswith('/embed'):
            inputs = body['input']
            if isinstance(inputs, str):
                inputs = [inputs]
            dimension = body.get('dimensions', 768)
            vectors = [embedding(text, dimension) for text in inputs]
            if self.path.endswith('/embed'):
                self.reply({'embeddings': vectors})
            else:
                self.reply({'data': [{'index': i, 'embedding': vectors[i]}
                                     for i in reversed(range(len(vectors)))]})
        elif self.path.endswith('/generate'):
            if body.get('stream') is not False:
                self.reply({'error': 'stream must be false'}, 400)
            else:
                self.reply({'response': body['prompt'], 'done': True})
        elif self.path.endswith('/rerank'):
            query = body['query'].lower()
            scores = [{'index': i, 'relevance_score': float(text.lower().count(query))}
                      for i, text in enumerate(body['documents'])]
            # Deliberately unsorted: the SQL implementation must sort and tie-break.
            self.reply({'results': scores[::-1]})
        elif self.path.endswith('/chat/completions'):
            self.reply({'choices': [{'message': {'content': body['messages'][-1]['content']}}]})
        else:
            self.reply({'error': 'unknown endpoint'}, 404)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    ThreadingHTTPServer(('0.0.0.0', args.port), Handler).serve_forever()
