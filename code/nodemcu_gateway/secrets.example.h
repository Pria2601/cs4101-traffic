// =====================================================================
// TEMPLATE - copy this file to secrets.h and fill it in.
// secrets.h is git-ignored because it holds the device private key.
// =====================================================================
#pragma once

const char WIFI_SSID[]     = "YOUR_WIFI_NAME";      // 2.4 GHz only
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

// IoT Core > Domain configurations > Domain name
const char AWS_IOT_ENDPOINT[] = "xxxxxxxxxxxxxx-ats.iot.ap-south-1.amazonaws.com";

// Must match the Thing name and the IoT policy
const char DEVICE_ID[] = "traffic_node_1";

// Amazon Root CA 1 (AmazonRootCA1.pem)
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE AmazonRootCA1.pem HERE
-----END CERTIFICATE-----
)EOF";

// Device certificate (xxxx-certificate.pem.crt)
static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE DEVICE CERTIFICATE HERE
-----END CERTIFICATE-----
)EOF";

// Private key (xxxx-private.pem.key)
static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
PASTE PRIVATE KEY HERE
-----END RSA PRIVATE KEY-----
)EOF";
