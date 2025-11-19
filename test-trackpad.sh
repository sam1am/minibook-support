#!/bin/bash

# Script to test trackpad events and capture logs

LOG_DIR="/tmp/moused-debug"
mkdir -p "$LOG_DIR"

MOUSED_BIN="./moused/bin/moused"

if [ ! -f "$MOUSED_BIN" ]; then
    echo "Error: moused binary not found at $MOUSED_BIN"
    echo "Please run 'make' in the moused directory first"
    exit 1
fi

# Check if moused service is running
if systemctl is-active --quiet moused; then
    echo "Stopping moused service..."
    sudo systemctl stop moused
    RESTART_SERVICE=1
else
    RESTART_SERVICE=0
fi

echo "Starting trackpad event tests..."
echo "Logs will be saved to: $LOG_DIR"
echo ""

# Function to run a test
run_test() {
    local test_name="$1"
    local test_description="$2"
    local log_file="$LOG_DIR/${test_name}.log"

    echo "================================================"
    echo "Test: $test_description"
    echo "================================================"
    echo "Starting moused in debug mode..."
    echo "Log file: $log_file"
    echo ""
    echo "Please perform the following action:"
    echo "  -> $test_description"
    echo ""
    echo "Press Enter when ready to start capturing..."
    read

    # Start moused in background and capture output
    sudo $MOUSED_BIN -d > "$log_file" 2>&1 &
    MOUSED_PID=$!

    echo "Capturing events... Perform the action now."
    echo "Press Enter when done..."
    read

    # Stop moused
    sudo kill $MOUSED_PID 2>/dev/null
    wait $MOUSED_PID 2>/dev/null

    echo "Log saved to: $log_file"
    echo ""
    sleep 1
}

# Test 1: Just moving the cursor
run_test "01-move-only" "Move your finger on the trackpad (no clicks) - just move the cursor around"

# Test 2: Tap to click
run_test "02-tap-click" "Do a tap-to-click (quickly tap the trackpad surface)"

# Test 3: Physical click
run_test "03-physical-click" "Do a physical click (press down on the trackpad button)"

# Test 4: Click and drag attempt
run_test "04-click-drag" "Try to click and drag (hold physical button down while moving)"

echo "================================================"
echo "All tests complete!"
echo "================================================"
echo ""
echo "Log files saved in: $LOG_DIR"
ls -lh "$LOG_DIR"
echo ""
echo "You can review the logs with:"
echo "  cat $LOG_DIR/01-move-only.log"
echo "  cat $LOG_DIR/02-tap-click.log"
echo "  cat $LOG_DIR/03-physical-click.log"
echo "  cat $LOG_DIR/04-click-drag.log"
echo ""

# Restart service if it was running
if [ $RESTART_SERVICE -eq 1 ]; then
    echo "Restarting moused service..."
    sudo systemctl start moused
fi

echo "Done!"
