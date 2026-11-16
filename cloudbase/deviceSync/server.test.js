const assert = require('node:assert/strict');
const http = require('node:http');
const test = require('node:test');
const { createServer } = require('./server');

test('HTTP server forwards device requests and enforces authentication', async () => {
  const server = createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();

  try {
    const response = await new Promise((resolve, reject) => {
      const request = http.request(
        {
          hostname: '127.0.0.1',
          port,
          path: '/deviceSync',
          method: 'POST',
          headers: { 'content-type': 'application/json' },
        },
        (result) => {
          let body = '';
          result.setEncoding('utf8');
          result.on('data', (chunk) => { body += chunk; });
          result.on('end', () => resolve({ statusCode: result.statusCode, body }));
        },
      );
      request.on('error', reject);
      request.end('{}');
    });

    assert.equal(response.statusCode, 401);
    assert.deepEqual(JSON.parse(response.body), { error: 'Unauthorized' });
  } finally {
    await new Promise((resolve, reject) => {
      server.close((error) => (error ? reject(error) : resolve()));
    });
  }
});
