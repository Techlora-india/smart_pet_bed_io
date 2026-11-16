const MAX_BODY_BYTES = 4096;
const REQUEST_TIMEOUT_MS = 3000;

class CloudBaseRestError extends Error {
  constructor(statusCode) {
    super(`CloudBase RDB REST request failed with status ${statusCode}.`);
    this.statusCode = statusCode;
  }
}

function badRequest(message) {
  return { statusCode: 400, body: JSON.stringify({ error: message }) };
}

function getConfiguration() {
  const envId = process.env.CLOUDBASE_ENV_ID?.trim();
  const apiKey = process.env.CLOUDBASE_API_KEY;
  if (!envId || !/^[a-z0-9-]+$/i.test(envId) || !apiKey) {
    return null;
  }

  return {
    apiBaseUrl: `https://${envId}.api.tcloudbasegateway.com`,
    apiKey,
  };
}

function getHeader(headers, name) {
  const value = Object.entries(headers || {}).find(
    ([key]) => key.toLowerCase() === name.toLowerCase(),
  )?.[1];
  return Array.isArray(value) ? value[0] : value;
}

function parsePayload(body) {
  if (typeof body !== 'string' || Buffer.byteLength(body, 'utf8') > MAX_BODY_BYTES) {
    return { error: 'Request body is missing or too large.' };
  }

  let payload;
  try {
    payload = JSON.parse(body);
  } catch {
    return { error: 'Request body must be valid JSON.' };
  }

  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) {
    return { error: 'Request body must be a JSON object.' };
  }

  const allowedFields = new Set([
    'deviceId',
    'userId',
    'currentTemp',
    'targetTemp',
    'powerState',
    'heaterOn',
    'mode',
    'petPresent',
    'firmwareVersion',
  ]);
  if (Object.keys(payload).some((field) => !allowedFields.has(field))) {
    return { error: 'Request body contains unsupported fields.' };
  }

  const deviceId = typeof payload.deviceId === 'string'
    ? payload.deviceId.trim().toUpperCase()
    : '';
  const firmwareVersion = payload.firmwareVersion;
  const currentTemp = payload.currentTemp;

  if (!/^[A-F0-9]{12}$/.test(deviceId) ||
      deviceId === '000000000000' ||
      deviceId === 'FFFFFFFFFFFF' ||
      !Number.isInteger(firmwareVersion) || firmwareVersion < 1 ||
      typeof payload.heaterOn !== 'boolean' ||
      typeof payload.petPresent !== 'boolean' ||
      (Object.hasOwn(payload, 'userId') &&
        (typeof payload.userId !== 'string' || payload.userId.length > 128)) ||
      (currentTemp !== null &&
        (typeof currentTemp !== 'number' || !Number.isFinite(currentTemp) ||
          currentTemp < -50 || currentTemp > 100)) ||
      (Object.hasOwn(payload, 'powerState') &&
        typeof payload.powerState !== 'boolean') ||
      (Object.hasOwn(payload, 'targetTemp') &&
        (typeof payload.targetTemp !== 'number' ||
          !Number.isFinite(payload.targetTemp) ||
          payload.targetTemp < 20 || payload.targetTemp > 42)) ||
      (Object.hasOwn(payload, 'mode') &&
        !['auto', 'heat', 'cool'].includes(payload.mode))) {
    return { error: 'Invalid device identity or telemetry fields.' };
  }

  // Legacy command and owner fields are accepted for wire compatibility only.
  return {
    value: {
      deviceId,
      firmwareVersion,
      currentTemp,
      heaterOn: payload.heaterOn,
    },
  };
}

async function rdbRequest(configuration, table, query, method = 'GET', body) {
  const url = `${configuration.apiBaseUrl}/v1/rdb/rest/${table}?${query}`;
  const response = await fetch(url, {
    method,
    headers: {
      accept: 'application/json',
      authorization: `Bearer ${configuration.apiKey}`,
      'content-type': 'application/json',
      ...(method !== 'GET' ? { prefer: 'return=representation' } : {}),
    },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
  });

  if (!response.ok) {
    throw new CloudBaseRestError(response.status);
  }

  const text = await response.text();
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    throw new Error('CloudBase RDB REST returned invalid JSON.');
  }
}

function serviceUnavailable(message) {
  return { statusCode: 503, body: JSON.stringify({ error: message }) };
}

exports.main = async (event) => {
  const apiKey = getHeader(event.headers, 'x-api-key');
  if (!apiKey || !process.env.DEVICE_SYNC_API_KEY ||
      apiKey !== process.env.DEVICE_SYNC_API_KEY) {
    return { statusCode: 401, body: JSON.stringify({ error: 'Unauthorized' }) };
  }

  if (event.method !== 'POST') {
    return {
      statusCode: 405,
      headers: { allow: 'POST' },
      body: JSON.stringify({ error: 'Method not allowed.' }),
    };
  }

  const parsed = parsePayload(event.body);
  if (parsed.error) return badRequest(parsed.error);

  const configuration = getConfiguration();
  if (!configuration) {
    return serviceUnavailable('Device sync database service is not configured.');
  }

  const { deviceId, firmwareVersion, currentTemp, heaterOn } = parsed.value;
  try {
    const telemetry = {
      firmware_version: firmwareVersion,
      current_temp: currentTemp,
      heater_on: heaterOn,
      last_seen: new Date().toISOString(),
    };
    const updateFilter = new URLSearchParams({
      device_id: `eq.${deviceId}`,
      select: 'device_id,target_temp,power_state,mode',
    });
    const updated = await rdbRequest(
      configuration,
      'pet_beds',
      updateFilter.toString(),
      'PATCH',
      telemetry,
    );
    const row = Array.isArray(updated) ? updated[0] : null;
    if (!row || row.device_id !== deviceId) {
      return {
        statusCode: 403,
        body: JSON.stringify({ error: 'Device is not registered.' }),
      };
    }

    const targetTemp = Number(row.target_temp);
    if (!Number.isFinite(targetTemp) ||
        typeof row.power_state !== 'boolean' ||
        !['auto', 'heat', 'cool'].includes(row.mode)) {
      throw new Error('CloudBase returned invalid desired-state data.');
    }

    return {
      statusCode: 200,
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        deviceId,
        desired: {
          targetTemp,
          powerState: row.power_state,
          mode: row.mode,
        },
      }),
    };
  } catch (error) {
    console.error('deviceSync RDB REST request failed', {
      statusCode: error.statusCode || null,
      errorName: error.name,
    });
    return serviceUnavailable('CloudBase database is unavailable.');
  }
};
