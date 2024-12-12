#!/bin/bash

# Check if URL is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <url>"
  exit 1
fi

URL=$1
CERT_DIR="main/certs"
CERT_FILE="ca_cert.pem"

# Create certs directory if it doesn't exist
mkdir -p $CERT_DIR

# Retrieve the CA certificates from the given URL and save to certs/ca_cert.pem
openssl s_client -showcerts -connect $URL:443 </dev/null 2>/dev/null | awk '/-----BEGIN CERTIFICATE-----/,/-----END CERTIFICATE-----/{print $0 "\n" > "'$CERT_DIR'/'$CERT_FILE'"}'

echo "CA certificates saved to $CERT_DIR/$CERT_FILE"
