#!/bin/bash

# Test specifically for physical click and drag issue

LOG_FILE="/tmp/moused-physical-click-drag.log"
MOUSED_BIN="./moused/bin/moused"

if [ ! -f "$MOUSED_BIN" ]; then
    echo "Error: moused binary not found at $MOUSED_BIN"
    echo "Please run 'make' in the moused directory first"
    exit 1
fi

echo "Testing Physical Click and Drag"
echo "================================"
echo ""
echo "Stopping moused service..."
sudo systemctl stop moused

echo ""
echo "Starting moused in debug mode..."
echo "Log file: $LOG_FILE"
echo ""
echo "IMPORTANT: Please do the following EXACT steps:"
echo ""
echo "1. DO NOT touch the trackpad yet"
echo "2. Press Enter to start capturing"
echo "3. Press and HOLD the physical click button"
echo "4. While holding the button, move your finger on the trackpad"
echo "5. Release the button"
echo "6. Press Enter to stop"
echo ""
read -p "Press Enter when ready to start..."

# Start moused in background
sudo $MOUSED_BIN -d > "$LOG_FILE" 2>&1 &
MOUSED_PID=$!

echo ""
echo "NOW: Press and hold the physical button, then try to move!"
echo ""
read -p "Press Enter when done..."

# Stop moused
sudo kill $MOUSED_PID 2>/dev/null
wait $MOUSED_PID 2>/dev/null

echo ""
echo "Log saved to: $LOG_FILE"
echo ""
echo "Restarting moused service..."
sudo systemctl start moused

echo ""
echo "Done! Log is ready for review."
