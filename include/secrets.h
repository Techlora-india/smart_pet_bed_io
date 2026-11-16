// Copy this file to include/secrets.h and replace the values for a local
// firmware build. Never commit secrets.h.
//
// The same values can instead be supplied with PlatformIO build_flags.
#define CLOUDBASE_SYNC_URL "https://disturbed-tahr-d3g08log18aceacb6-1528893789.ap-shanghai.app.tcloudbase.com/deviceSync"
#define DEVICE_SYNC_API_KEY "7942c6c839b7013994c8a263b1b06fa7a1a34861a257ac3e119fb7c76f5e525e"

// Provide the CA certificate for your CloudBase endpoint. Cloud sync is
// intentionally disabled when this value is empty.
#define CLOUDBASE_CA_CERT ""
