// Copy this file to include/secrets.h and replace the values for a local
// firmware build. Never commit secrets.h.
//
// The same values can instead be supplied with PlatformIO build_flags.
#define CLOUDBASE_SYNC_URL "https://replace-with-the-exact-function-trigger-url"
#define DEVICE_SYNC_API_KEY "replace-with-a-device-sync-key"

// Optional PEM trust-anchor override. Leave empty to use the built-in
// DigiCert Global Root G2 certificate for the CloudBase HTTPS endpoint.
#define CLOUDBASE_CA_CERT ""
