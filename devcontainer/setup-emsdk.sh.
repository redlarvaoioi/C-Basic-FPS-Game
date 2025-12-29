#!/bin/bash
set -e
# Install basic deps and Emscripten SDK (emsdk)
apt-get update
apt-get install -y git build-essential python3 python3-pip cmake wget unzip curl
# Clone emsdk if not present
if [ ! -d "/workspace/emsdk" ]; then
  git clone https://github.com/emscripten-core/emsdk.git /workspace/emsdk
  cd /workspace/emsdk
  ./emsdk install latest
  ./emsdk activate latest
else
  cd /workspace/emsdk
  ./emsdk install latest
  ./emsdk activate latest
fi
# Source environment for the remainder of the session
echo "source /workspace/emsdk/emsdk_env.sh" >> ~/.profile
source /workspace/emsdk/emsdk_env.sh
# Install emscripten-specific tools if not present (optional)
pip3 install --user wheel
echo "Emscripten setup complete. You may need to reload the Codespace or start a new terminal session."
