#!/bin/sh
# ak: AkIOS's build, boot and flash tool (tools/ak/); ./ak --help
cd "$(dirname "$0")/tools" && exec python3 -m ak "$@"
