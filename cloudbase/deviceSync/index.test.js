const assert = require('node:assert/strict');
const test = require('node:test');
const { main } = require('./index');

const configNames = [
  'CLOUDBASE_ENV_ID',
  'CLOUDBASE_API_KEY',
  'DEVICE_SYNC_API_KEY',
];

function configure(t, values = {}) {
  const previous = Object.fromEntries(
    configNames.map((name) => [name, process.env[name]]),
  );
  for (const name of configNames) {
    if (values[name] === undefined) delete process.env[name];
    else process.env[name] = values[name];
  }
  t.after(() => {
    for (const name of configNames) {
      if (previous[name] === undefined) delete process.env[name];
      else process.env[name] = previous[name];
    }
  });
}

function validPayload(overrides = {}) {
  return JSON.stringify({
    deviceId: '0123456789AB',
    userId: 'untrusted-owner',
    currentTemp: 25.5,
    targetTemp: 41,
    powerState: false,
    heaterOn: true,
    mode: 'cool',
    petPresent: false,
    firmwareVersion: 2,
    ...overrides,
  });
}

function authorizedEvent(overrides = {}) {
  return {
    method: 'POST',
    headers: { 'x-api-key': 'device-test-key' },
    body: validPayload(),
    ...overrides,
  };
}

function mockFetch(t, responses) {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, options) => {
    calls.push({ url: new URL(url), options });
    const next = responses.shift();
    assert.ok(next, 'unexpected CloudBase REST request');
    return new Response(JSON.stringify(next.body), {
      status: next.status || 200,
      headers: { 'content-type': 'application/json' },
    });
  };
  t.after(() => {
    globalThis.fetch = originalFetch;
  });
  return calls;
}

test('rejects missing and invalid device keys before database access', async (t) => {
  configure(t, { DEVICE_SYNC_API_KEY: 'device-test-key' });
  let fetchCalled = false;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => {
    fetchCalled = true;
    throw new Error('fetch must not be called');
  };
  t.after(() => { globalThis.fetch = originalFetch; });

  const missing = await main({ method: 'POST', headers: {}, body: '{}' });
  const invalid = await main({
    method: 'POST',
    headers: { 'x-api-key': 'wrong-key' },
    body: '{}',
  });
  assert.equal(missing.statusCode, 401);
  assert.equal(invalid.statusCode, 401);
  assert.equal(fetchCalled, false);
});

test('rejects non-POST requests and malformed payloads', async (t) => {
  configure(t, {
    DEVICE_SYNC_API_KEY: 'device-test-key',
    CLOUDBASE_ENV_ID: 'test-env',
    CLOUDBASE_API_KEY: 'server-test-key',
  });
  const getResponse = await main(authorizedEvent({ method: 'GET' }));
  const jsonResponse = await main(authorizedEvent({ body: '{' }));
  const shapeResponse = await main(authorizedEvent({ body: 'null' }));
  assert.equal(getResponse.statusCode, 405);
  assert.equal(jsonResponse.statusCode, 400);
  assert.equal(shapeResponse.statusCode, 400);
});

test('rejects invalid telemetry and unknown owner fields', async (t) => {
  configure(t, {
    DEVICE_SYNC_API_KEY: 'device-test-key',
    CLOUDBASE_ENV_ID: 'test-env',
    CLOUDBASE_API_KEY: 'server-test-key',
  });
  const invalidTelemetry = await main(
    authorizedEvent({ body: validPayload({ heaterOn: 'yes' }) }),
  );
  const unknownOwner = await main(
    authorizedEvent({ body: validPayload({ owner_id: 'attacker' }) }),
  );
  const zeroMac = await main(
    authorizedEvent({ body: validPayload({ deviceId: '000000000000' }) }),
  );
  assert.equal(invalidTelemetry.statusCode, 400);
  assert.equal(unknownOwner.statusCode, 400);
  assert.equal(zeroMac.statusCode, 400);
});

test('fails closed when the server-side CloudBase key is not configured', async (t) => {
  configure(t, {
    DEVICE_SYNC_API_KEY: 'device-test-key',
    CLOUDBASE_ENV_ID: 'test-env',
  });
  const response = await main(authorizedEvent());
  assert.equal(response.statusCode, 503);
});

test('patches registered telemetry only and returns desired state', async (t) => {
  configure(t, {
    CLOUDBASE_ENV_ID: 'test-env',
    CLOUDBASE_API_KEY: 'server-test-key',
    DEVICE_SYNC_API_KEY: 'device-test-key',
  });
  const calls = mockFetch(t, [{
    body: [{
      device_id: '0123456789AB',
      target_temp: 29.5,
      power_state: true,
      mode: 'heat',
    }],
  }]);

  const response = await main(authorizedEvent());
  assert.equal(response.statusCode, 200);
  assert.deepEqual(JSON.parse(response.body), {
    deviceId: '0123456789AB',
    desired: { targetTemp: 29.5, powerState: true, mode: 'heat' },
  });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url.origin, 'https://test-env.api.tcloudbasegateway.com');
  assert.equal(calls[0].url.pathname, '/v1/rdb/rest/pet_beds');
  assert.equal(
    calls[0].url.searchParams.get('select'),
    'device_id,target_temp,power_state,mode',
  );
  assert.equal(calls[0].url.searchParams.get('device_id'), 'eq.0123456789AB');
  assert.equal(calls[0].options.method, 'PATCH');
  assert.deepEqual(JSON.parse(calls[0].options.body), {
    firmware_version: 2,
    current_temp: 25.5,
    heater_on: true,
    last_seen: JSON.parse(calls[0].options.body).last_seen,
  });
  assert.ok(Number.isFinite(Date.parse(JSON.parse(calls[0].options.body).last_seen)));
  assert.equal(calls[0].options.headers.prefer, 'return=representation');
  for (const call of calls) {
    assert.equal(
      call.options.headers.authorization,
      'Bearer server-test-key',
    );
  }
});

test('refuses unregistered devices without creating a row', async (t) => {
  configure(t, {
    CLOUDBASE_ENV_ID: 'test-env',
    CLOUDBASE_API_KEY: 'server-test-key',
    DEVICE_SYNC_API_KEY: 'device-test-key',
  });
  const calls = mockFetch(t, [{ body: [] }]);

  const response = await main(authorizedEvent());
  assert.equal(response.statusCode, 403);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].options.method, 'PATCH');
});

test('does not write legacy owner or desired-state payload values', async (t) => {
  configure(t, {
    CLOUDBASE_ENV_ID: 'test-env',
    CLOUDBASE_API_KEY: 'server-test-key',
    DEVICE_SYNC_API_KEY: 'device-test-key',
  });
  const calls = mockFetch(t, [{
    body: [{
      device_id: '0123456789AB',
      target_temp: 30,
      power_state: true,
      mode: 'auto',
    }],
  }]);

  const response = await main(authorizedEvent());
  assert.equal(response.statusCode, 200);
  const telemetryUpdate = JSON.parse(calls[0].options.body);
  assert.equal('user_id' in telemetryUpdate, false);
  assert.equal('target_temp' in telemetryUpdate, false);
  assert.equal('power_state' in telemetryUpdate, false);
  assert.equal('mode' in telemetryUpdate, false);
  assert.equal('pet_present' in telemetryUpdate, false);
});
