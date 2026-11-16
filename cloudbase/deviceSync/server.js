const http = require('node:http');
const { main } = require('./index');

const MAX_BODY_BYTES = 4096;

function createServer() {
  return http.createServer((request, response) => {
    if (request.method === 'GET' && request.url.split('?')[0] === '/health') {
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end(JSON.stringify({ status: 'ok' }));
      return;
    }

    if (request.method !== 'POST') {
      response.writeHead(405, {
        allow: 'POST, GET',
        'content-type': 'application/json',
      });
      response.end(JSON.stringify({ error: 'Method not allowed.' }));
      return;
    }

    const chunks = [];
    let bodyBytes = 0;
    let tooLarge = false;

    request.on('data', (chunk) => {
      bodyBytes += chunk.length;
      if (bodyBytes > MAX_BODY_BYTES) {
        tooLarge = true;
        if (!response.headersSent) {
          response.writeHead(413, { 'content-type': 'application/json' });
          response.end(JSON.stringify({ error: 'Request body is too large.' }));
        }
        return;
      }
      chunks.push(chunk);
    });

    request.on('end', async () => {
      if (tooLarge) return;

      try {
        const result = await main({
          method: request.method,
          headers: request.headers,
          body: Buffer.concat(chunks).toString('utf8'),
        });
        response.writeHead(result.statusCode, {
          'content-type': 'application/json',
          ...result.headers,
        });
        response.end(result.body || '');
      } catch (error) {
        console.error('deviceSync HTTP handler failed', error);
        response.writeHead(500, { 'content-type': 'application/json' });
        response.end(JSON.stringify({ error: 'Internal server error.' }));
      }
    });
  });
}

if (require.main === module) {
  createServer().listen(9000, '0.0.0.0', () => {
    console.log('deviceSync listening on port 9000');
  });
}

module.exports = { createServer };
